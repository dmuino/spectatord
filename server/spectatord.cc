#include "spectatord.h"
#include "local_server.h"
#include "logger.h"
#include "proc_utils.h"
#include "spectator/percentile_distribution_summary.h"
#include "spectator/percentile_timer.h"
#include "spectator/registry.h"
#include "udp_server.h"

#include <asio.hpp>
#include <fmt/ostream.h>

namespace spectatord {

enum class StatsdMetricType { Counter, Timing, Gauge, Histogram, Set };

inline bool add_tag(spectator::Tags* tags, const char* begin_key,
                    const char* end_key, const char* begin_value,
                    const char* end_value) {
  std::string_view key;
  std::string_view value;

  if (begin_value != nullptr) {
    key = std::string_view{begin_key, static_cast<size_t>(end_key - begin_key)};
    value = std::string_view{begin_value,
                             static_cast<size_t>(end_value - begin_value)};
  } else {
    key =
        std::string_view{begin_key, static_cast<size_t>(end_value - begin_key)};
    value = "1";  // datadog statsd allows for marker tags, like #shell
                  // so we map them to shell=1
  }
  if (key.empty() || value.empty()) {
    return false;
  }
  tags->add(key, value);
  return true;
}

// feed lines into parser
std::optional<std::string> Server::parse_lines(char* buffer,
                                               const handler_t& parser) {
  char* p = buffer;
  std::string err_msg;
  while (*p != '\0') {
    char* newline = std::strchr(p, '\n');
    if (newline != nullptr) {
      *newline = '\0';
    }
    auto maybe_err = parser(p);
    if (maybe_err) {
      parse_errors_->Increment();
      if (err_msg.empty()) {
        err_msg = *maybe_err;
      } else {
        err_msg += '\n';
        err_msg += *maybe_err;
      }
    } else {
      parsed_count_->Increment();
    }
    if (newline == nullptr) {
      break;
    }
    // skip empty lines
    while (*++newline == '\n') {
    }
    p = newline;
  }
  if (err_msg.empty()) {
    return {};
  }

  return err_msg;
}

static void update_statsd_metric(spectator::Registry* registry,
                                 StatsdMetricType type, spectator::Id id,
                                 double value, double sampling_rate) {
  switch (type) {
    case StatsdMetricType::Counter:
      registry->GetCounter(std::move(id))->Add(value / sampling_rate);
      break;
    case StatsdMetricType::Gauge:
      // ignore sampling rate for gauges
      registry->GetGauge(std::move(id))->Set(value);
      break;
    case StatsdMetricType::Histogram: {
      auto k = std::lround(1 / sampling_rate);
      auto hist = registry->GetDistributionSummary(std::move(id));
      for (auto i = 0; i < k; ++i) {
        hist->Record(value);
      }
      break;
    }
    case StatsdMetricType::Timing: {
      auto k = std::lround(1 / sampling_rate);
      auto ns = std::chrono::nanoseconds(std::lround(value * 1e6));
      auto timer = registry->GetTimer(std::move(id));
      for (auto i = 0; i < k; ++i) {
        timer->Record(ns);
      }
      break;
    }
    case StatsdMetricType::Set:
      Logger()->info("Ignoring set cardinality metric for {}", id);
      break;
  }
}

// parse a single line of the form:
// <METRIC_NAME>:<VALUE>|<TYPE>|@<SAMPLE_RATE>|#<TAG_KEY_1>:<TAG_VALUE_1>,<TAG_2>
// See:
// https://docs.datadoghq.com/developers/dogstatsd/datagram_shell/?tab=metrics
// examples:
//  custom_metric:60|g|#shell - Set the custom_metric gauge to 60 and tag it
//  with 'shell' page.views:1|c - Increment the page.views COUNT metric.
//  fuel.level:0.5|g -  Record the fuel tank is half-empty.
//  song.length:240|h|@0.5 -  Sample the song.length histogram half of the time.
//  users.uniques:1234|s -  Track unique visitors to the site.
//  users.online:1|c|#country:china - Increment the active users COUNT metric
//  and tag by country of origin. users.online:1|c|@0.5|#country:china - Track
//  active China users and use a sample rate.
std::optional<std::string> Server::parse_statsd_line(const char* buffer) {
  assert(buffer != nullptr);

  // get name
  const char* p = std::strchr(buffer, ':');
  if (p == nullptr || p == buffer) {
    return "Invalid format: name is required";
  }
  std::string_view name{buffer, static_cast<size_t>(p - buffer)};

  // get value
  char* last_char = nullptr;
  ++p;
  auto value = std::strtod(p, &last_char);
  if (last_char == p) {
    return fmt::format("Unable to parse value starting at {}", p);
  }

  p = last_char;
  StatsdMetricType type;
  if (*p != '|') {
    return fmt::format("Invalid format. Expected | starting at {}", p);
  }
  ++p;
  char char_type = *p;
  switch (char_type) {
    case 'c':
      // counter
      type = StatsdMetricType::Counter;
      break;
    case 'g':
      type = StatsdMetricType::Gauge;
      break;
    case 'h':
      type = StatsdMetricType::Histogram;
      break;
    case 's':
      type = StatsdMetricType::Set;
      break;
    case 'm':
      type = StatsdMetricType::Timing;
      if (*++p != 's') {
        return fmt::format("Invalid metric type for {}", buffer);
      }
      break;
    default:
      return fmt::format("Invalid type for name {} ({})", name, buffer);
  }
  ++p;
  auto sampling_rate = 1.0;
  spectator::Tags tags{};
  if (*p == '|') {
    // maybe sampling rate
    ++p;
    if (*p == '@') {
      ++p;
      sampling_rate = std::strtod(p, &last_char);
      if (last_char == p || sampling_rate <= 0 || sampling_rate > 1) {
        return fmt::format("Invalid sampling rate for name={}", name);
      }
      p = last_char;
      if (*p == '|') {
        ++p;
      }
    }
    // maybe tags
    if (*p == '#') {
      ++p;
      const char* begin_key = p;
      const char* end_key = nullptr;
      const char* begin_value = nullptr;
      while (*p != '\0') {
        ++p;
        if (*p == ':') {
          end_key = p;
          begin_value = ++p;
        }
        if (*p == ',') {
          if (!add_tag(&tags, begin_key, end_key, begin_value, p)) {
            return fmt::format("Invalid tags for name={}", name);
          }
          begin_value = nullptr;
          begin_key = ++p;
        }
      }
      if (!add_tag(&tags, begin_key, end_key, begin_value, p)) {
        return fmt::format("Invalid tags for name={}", name);
      }
    }
  }

  spectator::Id id{spectator::intern_str(name), std::move(tags)};
  update_statsd_metric(registry_, type, std::move(id), value, sampling_rate);
  return {};
}

std::optional<measurement> get_measurement(const char* measurement_str,
                                           std::string* err_msg) {
  // get name (tags are specified with :# but are optional)
  const char* p = std::strchr(measurement_str, ':');
  if (p == nullptr || p == measurement_str) {
    *err_msg = "Missing name";
    return {};
  }
  std::string_view name{measurement_str,
                        static_cast<size_t>(p - measurement_str)};
  spectator::Tags tags{};

  ++p;
  // optionally get tags
  if (*p == '#') {
    while (*p != ':') {
      ++p;
      const char* k = std::strchr(p, '=');
      if (k == nullptr) break;
      std::string_view key{p, static_cast<size_t>(k - p)};
      ++k;
      p = std::strpbrk(k, ",:");
      if (p == nullptr) {
        *err_msg = "Missing value";
        return {};
      }
      std::string_view val{k, static_cast<size_t>(p - k)};
      tags.add(key, val);
    }
    ++p;
  }

  char* last_char = nullptr;
  auto value = std::strtod(p, &last_char);
  if (last_char == p) {
    // unable to parse a double
    *err_msg = "Unable to parse value for measurement";
    return {};
  }
  if (*last_char != '\0' && std::isspace(*last_char) == 0) {
    // just a warning
    *err_msg =
        fmt::format("Got {} parsing value, ignoring chars starting at {}",
                    value, last_char);
  }
  auto name_ref = spectator::intern_str(name);
  return measurement{spectator::Id{name_ref, tags}, value};
}

static constexpr auto min_perc_timer = absl::Nanoseconds(1);
static constexpr auto max_perc_timer = absl::Hours(24);
static constexpr auto min_ds = std::numeric_limits<int64_t>::min();
static constexpr auto max_ds = std::numeric_limits<int64_t>::max();

static auto create_perc_timer(spectator::Registry* registry, spectator::Id id) {
  return std::make_unique<spectator::PercentileTimer>(
      registry, std::move(id), min_perc_timer, max_perc_timer);
}

static auto create_perc_ds(spectator::Registry* registry, spectator::Id id) {
  return std::make_unique<spectator::PercentileDistributionSummary>(
      registry, std::move(id), min_ds, max_ds);
}
Server::Server(int port_number, int statsd_port_number, std::string socket_path,
               spectator::Registry* registry)
    : port_number_{port_number},
      statsd_port_number_{statsd_port_number},
      socket_path_{std::move(socket_path)},
      registry_{registry},
      parsed_count_{registry_->GetCounter("spectatord.parsedCount")},
      parse_errors_{registry_->GetCounter("spectatord.parseErrors")},
      logger_{Logger()},
      perc_timers_{create_perc_timer},
      perc_ds_{create_perc_ds} {}

void Server::Start() {
  auto logger = Logger();

  logger->info("Starting janitorial tasks");
  upkeep_thread_ = std::thread(&Server::upkeep, this);

  asio::io_context io_context;

  // stop the server on SIGINT / SIGTERM
  asio::signal_set signals(io_context, SIGINT, SIGTERM);
  signals.async_wait(
      [&io_context, this](std::error_code /*ec*/, int /*signo*/) {
        io_context.stop();
        this->Stop();
      });

  logger->info("Using receive buffer size = {}", max_buffer_size());
  auto parser = [this](char* buffer) { return this->parse(buffer); };
  UdpServer udp_server{io_context, port_number_, parser};
  logger->info("Starting spectatord server on port {}", port_number_);
  udp_server.Start();

  auto statsd_parser = [this](char* buffer) {
    return this->parse_statsd(buffer);
  };
  UdpServer statsd_server{io_context, statsd_port_number_, statsd_parser};
  logger->info("Starting statsd server on port {}", statsd_port_number_);
  statsd_server.Start();

  ::unlink(socket_path_.c_str());
  LocalServer dgram_local{io_context, socket_path_, parser};
  dgram_local.Start();
  logger->info("Starting local server (dgram) on socket {}", socket_path_);
  io_context.run();
}

// run our background tasks
void Server::upkeep() {
  static auto timers_size_gauge = registry_->GetGauge(
      "spectatord.percentileCacheSize", spectator::Tags{{"id", "timer"}});
  static auto ds_size_gauge =
      registry_->GetGauge("spectatord.percentileCacheSize",
                          spectator::Tags{{"id", "dist-summary"}});
  static auto timers_expired_ctr = registry_->GetCounter(
      "spectatord.percentileExpired", spectator::Tags{{"id", "timer"}});
  static auto ds_expired_ctr = registry_->GetCounter(
      "spectatord.percentileExpired", spectator::Tags{{"id", "dist-summary"}});

  static auto pool_hits = registry_->GetMonotonicCounter(
      "spectatord.poolAccess", spectator::Tags{{"id", "hit"}});
  static auto pool_misses = registry_->GetMonotonicCounter(
      "spectatord.poolAccess", spectator::Tags{{"id", "miss"}});
  static auto pool_alloc_size = registry_->GetGauge("spectatord.poolAllocSize");
  static auto pool_entries = registry_->GetGauge("spectatord.poolEntries");

  using clock = std::chrono::steady_clock;
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  static constexpr auto kFrequency = milliseconds{30000};
  auto logger = Logger();
  size_t ds_size = 0;
  size_t ds_expired = 0;
  size_t t_size = 0;
  size_t t_expired = 0;
  while (!should_stop_) {
    auto start = clock::now();
    std::tie(ds_size, ds_expired) = perc_ds_.expire();
    std::tie(t_size, t_expired) = perc_timers_.expire();
    timers_size_gauge->Set(t_size);
    ds_size_gauge->Set(ds_size);
    timers_expired_ctr->Add(t_expired);
    ds_expired_ctr->Add(ds_expired);
    update_network_metrics();
    auto pool_stats = spectator::string_pool_stats();
    pool_hits->Set(pool_stats.hits);
    pool_misses->Set(pool_stats.misses);
    pool_alloc_size->Set(pool_stats.alloc_size);
    pool_entries->Set(pool_stats.table_size);
    logger_->debug("Str Pool: Hits {} Misses {} Size {} Alloc {}",
                  pool_stats.hits, pool_stats.misses, pool_stats.table_size,
                  pool_stats.alloc_size);

    auto elapsed = clock::now() - start;
    auto millis = duration_cast<milliseconds>(elapsed);
    if (millis < kFrequency) {
      std::unique_lock<std::mutex> lock{cv_mutex_};
      auto sleep = kFrequency - elapsed;
      logger->debug("Janitor - sleeping {}ms",
                    duration_cast<milliseconds>(sleep).count());
      cv_.wait_for(lock, sleep);
    }
  }
}

void Server::update_network_metrics() {
  // parse /proc/net/udp to get dropped packets for our ports
  static auto udp_packets_dropped_ctr =
      registry_->GetMonotonicCounter("spectatord.udpPacketsDropped");
  static auto udp_queue = registry_->GetMaxGauge("spectatord.udpRxQueue");

  auto info = udp_info(port_number_);
  if (info) {
    udp_packets_dropped_ctr->Set(info->num_dropped);
    udp_queue->Set(info->rx_queue_bytes);
  }
}

void Server::Stop() {
  if (!should_stop_.exchange(true)) {
    logger_->debug("Stopping background tasks");
    cv_.notify_all();
    upkeep_thread_.join();
  }
}

std::optional<std::string> Server::parse_statsd(char* buffer) {
  return parse_lines(
      buffer, [this](char* line) { return this->parse_statsd_line(line); });
}

std::optional<std::string> Server::parse(char* buffer) {
  return parse_lines(
      buffer, [this](const char* line) { return this->parse_line(line); });
}

std::optional<std::string> Server::parse_line(const char* buffer) {
  static int_fast64_t parsed_count = 0;

  const char* p = buffer;
  auto version = *p++;
  if (version != '1') {
    return fmt::format("Unknown version: {} - ignoring message {}", version,
                       buffer);
  }

  if (*p++ != ':') {
    return "Expecting separator ':' at index 1";
  }

  auto type = *p++;
  auto ttl = absl::ZeroDuration();
  if (type == 'g' && *p == ',') {
    ++p;
    char* end_ttl = nullptr;
    auto ttl_secs = strtoul(p, &end_ttl, 10);
    if (ttl_secs <= 0) {
      return "Invalid ttl specified for gauge at index 3";
    }
    p = end_ttl;
    ttl = absl::Seconds(ttl_secs);
  }
  if (*p++ != ':') {
    return "Expecting separator ':' at index 3";
  }

  std::string err_msg;
  auto measurement = get_measurement(p, &err_msg);
  if (!measurement) {
    return err_msg;
  }

  if (!err_msg.empty()) {
    // got a warning while parsing
    Logger()->info("While parsing {}: {}", p, err_msg);
  }
  switch (type) {
    case 't':
      // timer, elapsed time is reported in seconds
      {
        auto nanos = static_cast<int64_t>(measurement->value * 1e9);
        registry_->GetTimer(measurement->id)
            ->Record(std::chrono::nanoseconds(nanos));
      }
      break;
    case 'c':
      // counter
      registry_->GetCounter(measurement->id)->Add(measurement->value);
      break;
    case 'C':
      // monotonic counters
      registry_->GetMonotonicCounter(measurement->id)->Set(measurement->value);
      break;
    case 'g':
      // gauge
      if (ttl != absl::ZeroDuration()) {
        registry_->GetGauge(measurement->id, ttl)->Set(measurement->value);
      } else {
        // this preserves the previous Ttl, otherwise we would override it
        // with the default value if we use the previous constructor
        registry_->GetGauge(measurement->id)->Set(measurement->value);
      }
      break;
    case 'm':
      registry_->GetMaxGauge(measurement->id)->Update(measurement->value);
      break;
    case 'd':
      // dist summary
      registry_->GetDistributionSummary(measurement->id)
          ->Record(measurement->value);
      break;
    case 'T': {
      auto nanos = static_cast<int64_t>(measurement->value * 1e9);
      perc_timers_.get_or_create(registry_, measurement->id)
          ->Record(std::chrono::nanoseconds(nanos));
    } break;
    case 'D':
      perc_ds_.get_or_create(registry_, measurement->id)
          ->Record(static_cast<int64_t>(measurement->value));
      break;
    default:
      return fmt::format("Unknown type: {}", type);
  }

  ++parsed_count;
  if (parsed_count % 50000 == 0) {
    logger_->debug("Parsed {} messages", parsed_count);
    logger_->debug("Meters in Registry = {}", registry_->Size());
  }
  return {};
}

}  // namespace spectatord
