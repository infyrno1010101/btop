// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include <map>

namespace AiMetrics {
struct Values {
	std::optional<double> generation_tps;
	std::optional<double> generated_tokens;
	std::optional<double> generation_seconds;
	std::optional<double> active;
	std::optional<double> queued;
	std::optional<double> prompt_tokens;
	std::optional<double> process_start;
	std::map<std::string, double> generation_series;
};
struct Slot {
	uint64_t id = 0;
	uint64_t task = 0;
	uint64_t decoded = 0;
	std::optional<uint64_t> prompt_processed;
	bool processing = false;
};
struct LiveValues {
	std::optional<double> tps;
	uint64_t output_tokens = 0;
	std::optional<uint64_t> prompt_processed = 0;
	std::string status = "disabled";
	uint64_t active_slots = 0;
	bool enabled = false;
	bool available = false;
};
struct SlotSnapshot {
	std::vector<Slot> slots;
	std::string status;
	bool available = false;
	std::optional<uint64_t> server_instance;
};
// Per-poller state; rates never cross endpoint/key changes or failed scrapes.
class LiveTracker {
	std::vector<Slot> previous;
	std::chrono::steady_clock::time_point previous_time{};
	bool primed = false;
	std::optional<uint64_t> previous_instance;
public:
	auto update(const SlotSnapshot& sample, std::chrono::steady_clock::time_point now) -> LiveValues;
};
struct HistoryPoint {
	std::optional<double> tps;
	std::optional<double> queued;
};
struct ProcessSample {
	size_t pid = 0;
	size_t workload_root = 0;
	double gpu_percent = 0;
	uint64_t gpu_memory = 0;
};
struct ProcessUsage {
	size_t configured_pid = 0;
	size_t root_pid = 0;
	size_t process_count = 0;
	double gpu_percent = 0;
	uint64_t gpu_memory = 0;
	bool configured = false;
	bool available = false;
};
struct Snapshot {
	std::string endpoint;
	std::string status = "disabled";
	Values values;
	LiveValues live;
	bool available = false;
	std::optional<uint64_t> server_instance;
	std::string server_name;
	size_t server_index = 0;
	size_t server_count = 0;
	std::deque<HistoryPoint> history;
	std::string runtime = "llama.cpp";
	size_t loaded_models = 0;
	uint64_t model_vram = 0;
	std::string model_name;
	ProcessUsage process;
};
class CounterTracker {
	Values previous;
	std::optional<uint64_t> previous_instance;
	std::chrono::steady_clock::time_point previous_time{};
	bool primed = false;
public:
	auto update(const Snapshot& sample, std::chrono::steady_clock::time_point now) -> LiveValues;
};
class History {
	std::deque<HistoryPoint> points;
public:
	void append(const Snapshot& sample);
	auto values() const -> const std::deque<HistoryPoint>& { return points; }
};
// Five plain-text chart rows. One column represents one completed source poll.
auto history_chart(const std::deque<HistoryPoint>& points, int width, bool tty = false) -> std::vector<std::string>;
struct Connection {
	std::string name;
	std::string endpoint;
	std::string key_file;
	bool live = true;
	std::string runtime = "llama.cpp";
	size_t process_pid = 0;
	bool operator==(const Connection&) const = default;
};
auto parse_connections(std::string_view body, bool default_live = true) -> std::optional<std::vector<Connection>>;
auto loopback_port(std::string_view endpoint) -> std::optional<uint16_t>;
auto parse_prometheus(std::string_view body) -> std::optional<Values>;
auto parse_vllm(std::string_view body) -> std::optional<Values>;
auto parse_sglang(std::string_view body) -> std::optional<Values>;
auto parse_ollama(std::string_view body) -> std::optional<Snapshot>;
auto parse_slots(std::string_view body) -> std::optional<std::vector<Slot>>;
auto fetch_llama(std::string_view endpoint, std::chrono::milliseconds budget = std::chrono::milliseconds(250), std::string_view key_file = {}) -> Snapshot;
auto fetch_vllm(std::string_view endpoint, std::chrono::milliseconds budget = std::chrono::milliseconds(250), std::string_view key_file = {}) -> Snapshot;
auto fetch_sglang(std::string_view endpoint, std::chrono::milliseconds budget = std::chrono::milliseconds(250), std::string_view key_file = {}) -> Snapshot;
auto fetch_ollama(std::string_view endpoint, std::chrono::milliseconds budget = std::chrono::milliseconds(250), std::string_view key_file = {}) -> Snapshot;
auto fetch_slots(std::string_view endpoint, std::chrono::milliseconds budget = std::chrono::milliseconds(250), std::string_view key_file = {}) -> SlotSnapshot;
auto summary(const Snapshot& sample) -> std::string;
// Seven rows hold a border and five content lines, leaving room for processes.
auto panel_rows(int width, int available_height, bool enabled) -> int;
auto panel_lines(const Snapshot& sample) -> std::vector<std::string>;
auto aggregate_processes(size_t configured_pid, const std::vector<ProcessSample>& processes) -> ProcessUsage;
// Configuration never performs I/O on the drawing/collection thread.
void configure(const std::string& endpoint, const std::string& key_file = {}, bool live = true, const std::string& servers_file = {});
auto snapshot() -> Snapshot;
auto snapshots() -> std::vector<Snapshot>;
void update_processes(const std::vector<ProcessSample>& processes);
void cycle_server(int direction);
void shutdown();
}
