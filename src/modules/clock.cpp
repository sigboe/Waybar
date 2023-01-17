#include "modules/clock.hpp"

#include <fmt/chrono.h>
#include <spdlog/spdlog.h>

#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>
#include <type_traits>

#include "util/date.hpp"
#include "util/ustring_clen.hpp"
#ifdef HAVE_LANGINFO_1STDAY
#include <langinfo.h>
#include <locale.h>
#endif

waybar::modules::Clock::Clock(const std::string& id, const Json::Value& config)
    : ALabel(config, "clock", id, "{:%H:%M}", 60, false, false, true),
      current_time_zone_idx_(0),
      is_calendar_in_tooltip_(false),
      is_timezoned_list_in_tooltip_(false) {
  if (config_["timezones"].isArray() && !config_["timezones"].empty()) {
    for (const auto& zone_name : config_["timezones"]) {
      if (!zone_name.isString() || zone_name.asString().empty()) {
        time_zones_.push_back(nullptr);
        continue;
      }
      time_zones_.push_back(date::locate_zone(zone_name.asString()));
    }
  } else if (config_["timezone"].isString() && !config_["timezone"].asString().empty()) {
    time_zones_.push_back(date::locate_zone(config_["timezone"].asString()));
  }

  // If all timezones are parsed and no one is good, add nullptr to the timezones vector, to mark
  // that local time should be shown.
  if (!time_zones_.size()) {
    time_zones_.push_back(nullptr);
  }

  // Check if a particular placeholder is present in the tooltip format, to know what to calculate
  // on update.
  if (config_["tooltip-format"].isString()) {
    std::string trimmed_format = config_["tooltip-format"].asString();
    trimmed_format.erase(std::remove_if(trimmed_format.begin(), trimmed_format.end(),
                                        [](unsigned char x) { return std::isspace(x); }),
                         trimmed_format.end());
    if (trimmed_format.find("{" + kCalendarPlaceholder + "}") != std::string::npos) {
      is_calendar_in_tooltip_ = true;
    }
    if (trimmed_format.find("{" + KTimezonedTimeListPlaceholder + "}") != std::string::npos) {
      is_timezoned_list_in_tooltip_ = true;
    }
  }

  if (is_calendar_in_tooltip_) {
    if (config_["on-scroll"][kCalendarPlaceholder].isInt()) {
      calendar_shift_init_ =
          date::months{config_["on-scroll"].get(kCalendarPlaceholder, 0).asInt()};
      event_box_.add_events(Gdk::LEAVE_NOTIFY_MASK);
      event_box_.signal_leave_notify_event().connect([this](GdkEventCrossing*) {
        calendar_shift_ = date::months{0};
        return false;
      });
    }
  }

  if (config_["locale"].isString()) {
    locale_ = std::locale(config_["locale"].asString());
  } else {
    locale_ = std::locale("");
  }

  if (config_["format-calendar-weeks"].isString()) {
    fmt_str_weeks_ =
        std::regex_replace(config_["format-calendar-weeks"].asString(), std::regex("\\{\\}"),
                           (first_day_of_week() == date::Monday) ? "{:%V}" : "{:%U}");
    fmt_weeks_left_pad_ =
        std::regex_replace(fmt_str_weeks_, std::regex("</?[^>]+>|\\{.*\\}"), "").length();
  } else {
    fmt_str_weeks_ = "";
  }

  if (config_["format-calendar"].isString()) {
    fmt_str_calendar_ = config_["format-calendar"].asString();
  } else {
    fmt_str_calendar_ = "{}";
  }

  thread_ = [this] {
    dp.emit();
    auto now = std::chrono::system_clock::now();
    /* difference with projected wakeup time */
    auto diff = now.time_since_epoch() % interval_;
    /* sleep until the next projected time */
    thread_.sleep_for(interval_ - diff);
  };
}

const date::time_zone* waybar::modules::Clock::current_timezone() {
  return time_zones_[current_time_zone_idx_] ? time_zones_[current_time_zone_idx_]
                                             : date::current_zone();
}

bool waybar::modules::Clock::is_timezone_fixed() {
  return time_zones_[current_time_zone_idx_] != nullptr;
}

auto waybar::modules::Clock::update() -> void {
  const auto* time_zone = current_timezone();
  auto now = std::chrono::system_clock::now();
  auto ztime = date::zoned_time{time_zone, date::floor<std::chrono::seconds>(now)};

  auto shifted_date = date::year_month_day{date::floor<date::days>(now)} + calendar_shift_;
  auto now_shifted = date::sys_days{shifted_date} + (now - date::floor<date::days>(now));
  auto shifted_ztime = date::zoned_time{time_zone, date::floor<std::chrono::seconds>(now_shifted)};

  std::string text = "";
  if (!is_timezone_fixed()) {
    // As date dep is not fully compatible, prefer fmt
    tzset();
    auto localtime = fmt::localtime(std::chrono::system_clock::to_time_t(now));
    text = fmt::format(locale_, format_, localtime);
  } else {
    text = fmt::format(locale_, format_, ztime);
  }
  label_.set_markup(text);

  if (tooltipEnabled()) {
    if (config_["tooltip-format"].isString()) {
      std::string calendar_lines{""};
      std::string timezoned_time_lines{""};
      if (is_calendar_in_tooltip_) {
        calendar_lines = calendar_text(shifted_ztime);
      }
      if (is_timezoned_list_in_tooltip_) {
        timezoned_time_lines = timezones_text(&now);
      }
      auto tooltip_format = config_["tooltip-format"].asString();
      text = fmt::format(locale_, tooltip_format, shifted_ztime,
                         fmt::arg(kCalendarPlaceholder.c_str(), calendar_lines),
                         fmt::arg(KTimezonedTimeListPlaceholder.c_str(), timezoned_time_lines));
      label_.set_tooltip_markup(text);
    }
  }

  // Call parent update
  ALabel::update();
}

bool waybar::modules::Clock::handleScroll(GdkEventScroll* e) {
  // defer to user commands if set
  if (config_["on-scroll-up"].isString() || config_["on-scroll-down"].isString()) {
    return AModule::handleScroll(e);
  }

  auto dir = AModule::getScrollDir(e);

  // Shift calendar date
  if (calendar_shift_init_.count() != 0) {
    if (dir == SCROLL_DIR::UP)
      calendar_shift_ += calendar_shift_init_;
    else
      calendar_shift_ -= calendar_shift_init_;
  } else {
    // Change time zone
    if (dir != SCROLL_DIR::UP && dir != SCROLL_DIR::DOWN) {
      return true;
    }
    if (time_zones_.size() == 1) {
      return true;
    }

    auto nr_zones = time_zones_.size();
    if (dir == SCROLL_DIR::UP) {
      size_t new_idx = current_time_zone_idx_ + 1;
      current_time_zone_idx_ = new_idx == nr_zones ? 0 : new_idx;
    } else {
      current_time_zone_idx_ =
          current_time_zone_idx_ == 0 ? nr_zones - 1 : current_time_zone_idx_ - 1;
    }
  }

  update();
  return true;
}

auto waybar::modules::Clock::calendar_text(const date::zoned_seconds& ztime) -> std::string {
  const auto daypoint = date::floor<date::days>(ztime.get_local_time());
  const auto ymd{date::year_month_day{daypoint}};

  if (calendar_cached_ymd_ == ymd) {
    return calendar_cached_text_;
  }

  const auto curr_day{(calendar_shift_init_.count() != 0 && calendar_shift_.count() != 0)
                          ? date::day{0}
                          : ymd.day()};
  const date::year_month ym{ymd.year(), ymd.month()};
  const auto first_dow = first_day_of_week();

  std::stringstream os;

  enum class WeeksSide {
    LEFT,
    RIGHT,
    HIDDEN,
  };
  WeeksSide weeks_pos = WeeksSide::HIDDEN;

  if (config_["calendar-weeks-pos"].isString()) {
    if (config_["calendar-weeks-pos"].asString() == "left") {
      weeks_pos = WeeksSide::LEFT;
      // Add paddings before the header
      os << std::string(3 + fmt_weeks_left_pad_, ' ');
    } else if (config_["calendar-weeks-pos"].asString() == "right") {
      weeks_pos = WeeksSide::RIGHT;
    }
  }

  weekdays_header(first_dow, os);

  // First week day prefixed with spaces if needed.
  date::sys_days print_wd{ym / 1};
  auto wd{date::weekday{print_wd}};
  auto empty_days = (wd - first_dow).count();

  /* Print weeknumber on the left for the first row*/
  if (weeks_pos == WeeksSide::LEFT) {
    os << fmt::format(fmt_str_weeks_, print_wd) << ' ';
  }

  if (empty_days > 0) {
    os << std::string(empty_days * 3 - 1, ' ');
  }

  const auto last_day = (ym / date::literals::last).day();

  for (auto d{date::day{1}}; d <= last_day; ++d, ++wd) {
    if (wd != first_dow) {
      os << ' ';
    } else if (unsigned(d) != 1) {
      if (weeks_pos == WeeksSide::RIGHT) {
        os << ' ' << fmt::format(fmt_str_weeks_, print_wd);
      }

      os << '\n';

      print_wd = (ym / d);

      if (weeks_pos == WeeksSide::LEFT) {
        os << fmt::format(fmt_str_weeks_, print_wd) << ' ';
      }
    }

    if (d == curr_day) {
      if (config_["today-format"].isString()) {
        auto today_format = config_["today-format"].asString();
        os << fmt::format(today_format, date::format("%e", d));
      } else {
        os << "<b><u>" << date::format("%e", d) << "</u></b>";
      }
    } else {
      os << fmt::format(fmt_str_calendar_, date::format("%e", d));
    }
    /*Print weeks on the right when the endings with spaces*/
    if (weeks_pos == WeeksSide::RIGHT && d == last_day) {
      empty_days = 6 - (wd.c_encoding() - first_dow.c_encoding());
      if (empty_days > 0 && empty_days < 7) {
        os << std::string(empty_days * 3, ' ');
      }

      os << ' ' << fmt::format(fmt_str_weeks_, print_wd);
    }
  }

  auto result = os.str();
  calendar_cached_ymd_ = ymd;
  calendar_cached_text_ = result;
  return result;
}

auto waybar::modules::Clock::weekdays_header(const date::weekday& first_week_day, std::ostream& os)
    -> void {
  std::stringstream res;
  auto wd = first_week_day;
  do {
    if (wd != first_week_day) {
      res << ' ';
    }
    Glib::ustring wd_ustring(date::format(locale_, "%a", wd));
    auto clen = ustring_clen(wd_ustring);
    auto wd_len = wd_ustring.length();
    while (clen > 2) {
      wd_ustring = wd_ustring.substr(0, wd_len - 1);
      wd_len--;
      clen = ustring_clen(wd_ustring);
    }
    const std::string pad(2 - clen, ' ');
    res << pad << wd_ustring;
  } while (++wd != first_week_day);
  res << '\n';

  if (config_["format-calendar-weekdays"].isString()) {
    os << fmt::format(config_["format-calendar-weekdays"].asString(), res.str());
  } else
    os << res.str();
}

auto waybar::modules::Clock::timezones_text(std::chrono::system_clock::time_point* now)
    -> std::string {
  if (time_zones_.size() == 1) {
    return "";
  }
  std::stringstream os;
  for (size_t time_zone_idx = 0; time_zone_idx < time_zones_.size(); ++time_zone_idx) {
    if (static_cast<int>(time_zone_idx) == current_time_zone_idx_) {
      continue;
    }
    const date::time_zone* timezone = time_zones_[time_zone_idx];
    if (!timezone) {
      timezone = date::current_zone();
    }
    auto ztime = date::zoned_time{timezone, date::floor<std::chrono::seconds>(*now)};
    os << fmt::format(locale_, format_, ztime) << '\n';
  }
  return os.str();
}

#ifdef HAVE_LANGINFO_1STDAY
template <auto fn>
using deleter_from_fn = std::integral_constant<decltype(fn), fn>;

template <typename T, auto fn>
using deleting_unique_ptr = std::unique_ptr<T, deleter_from_fn<fn>>;
#endif

// Computations done similarly to Linux cal utility.
auto waybar::modules::Clock::first_day_of_week() -> date::weekday {
#ifdef HAVE_LANGINFO_1STDAY
  deleting_unique_ptr<std::remove_pointer<locale_t>::type, freelocale> posix_locale{
      newlocale(LC_ALL, locale_.name().c_str(), nullptr)};
  if (posix_locale) {
    const int i = (std::intptr_t)nl_langinfo_l(_NL_TIME_WEEK_1STDAY, posix_locale.get());
    auto ymd = date::year(i / 10000) / (i / 100 % 100) / (i % 100);
    auto wd = date::weekday(ymd);
    uint8_t j = *nl_langinfo_l(_NL_TIME_FIRST_WEEKDAY, posix_locale.get());
    return wd + date::days(j - 1);
  }
#endif
  return date::Sunday;
}
