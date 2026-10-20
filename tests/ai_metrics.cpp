// SPDX-License-Identifier: Apache-2.0
#include "btop_ai_metrics.hpp"
#include "btop_config.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <cstdlib>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#endif
using namespace std::chrono_literals;

TEST(ai_metrics, endpoint_validation) {
	EXPECT_EQ(AiMetrics::loopback_port("127.0.0.1:8080"), 8080);
	EXPECT_EQ(AiMetrics::loopback_port("localhost:65535"), 65535);
	for (auto invalid : {"", "127.0.0.1:0", "127.0.0.1:65536", "127.0.0.1:-1", "127.0.0.1:+80",
		"127.0.0.1:8080/metrics", "localhost:80\r\nOther: value", "http://localhost:80",
		"example.org:80", "192.168.1.1:80", "127.0.0.1.evil:80", "localhost:80x"})
		EXPECT_FALSE(AiMetrics::loopback_port(invalid)) << invalid;
	EXPECT_TRUE(Config::stringValid("ai_llama_endpoint", ""));
	EXPECT_FALSE(Config::stringValid("ai_llama_endpoint", "example.org:80"));
}

TEST(ai_metrics, parses_exporter_metrics) {
	const auto values = AiMetrics::parse_prometheus(
		"# TYPE llamacpp:predicted_tokens_seconds gauge\n"
		"llamacpp:predicted_tokens_seconds 4.25e1\n"
		"llamacpp:requests_processing{model=\"test}model\"} 2\n"
		"llamacpp:requests_deferred 0 1700000000000\n"
		"unrelated_metric 123\n");
	ASSERT_TRUE(values);
	EXPECT_EQ(values->generation_tps, 42.5);
	EXPECT_EQ(values->active, 2);
	EXPECT_EQ(values->queued, 0);
}

TEST(ai_metrics, missing_metrics_are_not_zero) {
	const auto values = AiMetrics::parse_prometheus("llamacpp:requests_processing 0\n");
	ASSERT_TRUE(values);
	EXPECT_FALSE(values->generation_tps);
	EXPECT_EQ(values->active, 0);
	EXPECT_FALSE(values->queued);
	AiMetrics::Snapshot snapshot;
	snapshot.available = true;
	snapshot.values = *values;
	const auto text = AiMetrics::summary(snapshot);
	EXPECT_NE(text.find("gen(avg) N/A"), text.npos);
	EXPECT_NE(text.find("active 0"), text.npos);
	EXPECT_NE(text.find("queue N/A"), text.npos);
}

TEST(ai_metrics, zero_gauge_uses_accumulated_generation_average) {
	const auto values = AiMetrics::parse_prometheus(
		"llamacpp:predicted_tokens_seconds 0\n"
		"llamacpp:tokens_predicted_total 7470\n"
		"llamacpp:tokens_predicted_seconds_total 227.973\n"
		"llamacpp:requests_processing 1\nllamacpp:requests_deferred 0\n");
	ASSERT_TRUE(values);
	EXPECT_EQ(values->generated_tokens, 7470);
	EXPECT_EQ(values->generation_seconds, 227.973);
	AiMetrics::Snapshot sample;
	sample.available = true;
	sample.values = *values;
	auto text = AiMetrics::summary(sample);
	EXPECT_NE(text.find("gen(total) 32.8 tok/s"), text.npos);
	EXPECT_NE(text.find("tokens 7470"), text.npos);
	EXPECT_NE(text.find("active 1"), text.npos);
	// A valid nonzero exporter gauge still takes priority.
	sample.values.generation_tps = 40.5;
	EXPECT_NE(AiMetrics::summary(sample).find("gen(avg) 40.5 tok/s"), text.npos);
	// A missing gauge can also use counters, without a previous scrape.
	sample.values.generation_tps.reset();
	EXPECT_NE(AiMetrics::summary(sample).find("gen(total) 32.8 tok/s"), text.npos);
}

TEST(ai_metrics, unknown_generation_speed_is_not_a_false_zero) {
	AiMetrics::Snapshot sample;
	sample.available = true;
	sample.values.generation_tps = 0;
	sample.values.active = 1;
	sample.values.generated_tokens = 0;
	sample.values.generation_seconds = 0;
	EXPECT_NE(AiMetrics::summary(sample).find("gen(avg) N/A"), std::string::npos);
	sample.values.generated_tokens = 100;
	EXPECT_NE(AiMetrics::summary(sample).find("gen(avg) N/A"), std::string::npos);
	sample.values.generation_seconds.reset();
	EXPECT_NE(AiMetrics::summary(sample).find("gen(avg) N/A"), std::string::npos);
	// A server restart does not carry a previous counter average forward.
	sample.values.generated_tokens = 0;
	sample.values.generation_seconds = 0;
	EXPECT_EQ(AiMetrics::summary(sample).find("gen(total)"), std::string::npos);
}

TEST(ai_metrics, validates_generation_counters) {
	for (const auto body : {"llamacpp:tokens_predicted_total -1\n",
		"llamacpp:tokens_predicted_total 1.5\n",
		"llamacpp:tokens_predicted_seconds_total NaN\n",
		"llamacpp:tokens_predicted_seconds_total -0.1\n",
		"llamacpp:tokens_predicted_total 1\nllamacpp:tokens_predicted_total 2\n"})
		EXPECT_FALSE(AiMetrics::parse_prometheus(body)) << body;
}

TEST(ai_metrics, rejects_invalid_or_ambiguous_metrics) {
	for (const auto body : {"", "unrelated 1\n", "llamacpp:requests_processing NaN\n",
		"llamacpp:requests_processing -1\n", "llamacpp:requests_processing 1.5\n",
		"llamacpp:predicted_tokens_seconds Inf\n", "llamacpp:predicted_tokens_seconds 1oops\n",
		"llamacpp:requests_processing\n", "llamacpp:requests_processing{model=\"missing} 1\n",
		"llamacpp:requests_processing 1\nllamacpp:requests_processing 2\n"})
		EXPECT_FALSE(AiMetrics::parse_prometheus(body)) << body;
}

TEST(ai_metrics, parses_slots_without_retaining_request_text) {
	const auto slots = AiMetrics::parse_slots(R"([
		{"id":0,"is_processing":false},
		{"id":1,"id_task":42,"is_processing":true,"n_prompt_tokens_processed":120,
		 "prompt":"private input","generated":"private output",
		 "next_token":[{"n_decoded":75}]},
		{"id":2,"id_task":43,"is_processing":true,"next_token":{"n_decoded":9}}
	])");
	ASSERT_TRUE(slots);
	ASSERT_EQ(slots->size(), 3);
	EXPECT_FALSE(slots->at(0).processing);
	EXPECT_EQ(slots->at(1).task, 42);
	EXPECT_EQ(slots->at(1).decoded, 75);
	EXPECT_EQ(slots->at(1).prompt_processed, 120);
	EXPECT_EQ(slots->at(2).decoded, 9);
	EXPECT_TRUE(AiMetrics::parse_slots("[]"));
}

TEST(ai_metrics, rejects_malformed_or_unsupported_slot_data) {
	for (const auto body : {"{}", "[", "[1]", R"([{"id":0}])",
		R"([{"id":0,"is_processing":1}])",
		R"([{"id":0,"is_processing":true,"next_token":{"n_decoded":2}}])",
		R"([{"id":0,"id_task":1,"is_processing":true,"next_token":[]}])",
		R"([{"id":0,"id_task":1,"is_processing":true,"next_token":{"n_decoded":-1}}])",
		R"([{"id":0,"id_task":1,"is_processing":true,"next_token":{"n_decoded":1.2}}])",
		R"([{"id":0,"is_processing":false},{"id":0,"is_processing":false}])"})
		EXPECT_FALSE(AiMetrics::parse_slots(body)) << body;
	EXPECT_FALSE(AiMetrics::parse_slots(std::string(33, '[') + std::string(33, ']')));
	EXPECT_FALSE(AiMetrics::parse_slots(std::string(256 * 1024 + 1, ' ')));
}

TEST(ai_metrics, live_rate_tracks_request_identity_and_recovers_from_failures) {
	AiMetrics::LiveTracker tracker;
	const auto start = std::chrono::steady_clock::time_point{};
	AiMetrics::SlotSnapshot sample;
	sample.available = true;
	sample.slots = {{0, 41, 100, 200, true}};
	EXPECT_FALSE(tracker.update(sample, start).tps); // first sample needs a baseline
	sample.slots[0].decoded = 180;
	const auto live = tracker.update(sample, start + 2s);
	EXPECT_EQ(live.tps, 40);
	EXPECT_EQ(live.output_tokens, 180);
	EXPECT_EQ(live.prompt_processed, 200);
	EXPECT_EQ(live.active_slots, 1);
	// Same slot, new task: even a larger count cannot become a rate spike.
	sample.slots[0].task = 42;
	sample.slots[0].decoded = 500;
	EXPECT_FALSE(tracker.update(sample, start + 4s).tps);
	sample.slots[0].decoded = 550;
	EXPECT_EQ(tracker.update(sample, start + 6s).tps, 25);
	sample.slots[0].decoded = 0; // reset of the same task's counters
	EXPECT_FALSE(tracker.update(sample, start + 8s).tps);
	AiMetrics::SlotSnapshot failure;
	failure.status = "timeout";
	const auto failed = tracker.update(failure, start + 10s);
	EXPECT_FALSE(failed.available);
	EXPECT_FALSE(failed.tps);
	EXPECT_EQ(failed.output_tokens, 0);
	EXPECT_FALSE(tracker.update(sample, start + 12s).tps);
	EXPECT_FALSE(tracker.update(sample, start + 24s).tps); // no stale long-gap rates
	sample.server_instance = 123;
	EXPECT_FALSE(tracker.update(sample, start + 25s).tps); // changed server start time
	sample.slots[0].processing = false;
	const auto idle = tracker.update(sample, start + 26s);
	EXPECT_EQ(idle.tps, 0);
	EXPECT_EQ(idle.output_tokens, 0);
}

TEST(ai_metrics, live_rate_reports_prompt_processing_before_generation) {
	AiMetrics::LiveTracker tracker;
	const auto start = std::chrono::steady_clock::time_point{};
	AiMetrics::SlotSnapshot sample;
	sample.available = true;
	sample.slots = {{0, 41, 0, 200, true}};
	tracker.update(sample, start);
	sample.slots[0].prompt_processed = 500;
	const auto prompt = tracker.update(sample, start + 2s);
	EXPECT_EQ(prompt.tps, 0);
	EXPECT_EQ(prompt.prompt_tps, 150);
	AiMetrics::Snapshot panel;
	panel.available = true;
	panel.live = prompt;
	EXPECT_EQ(AiMetrics::panel_lines(panel)[1], "Live: 150.0 tok/s (prompt)");
	sample.slots[0].decoded = 80;
	const auto generation = tracker.update(sample, start + 4s);
	EXPECT_EQ(generation.tps, 40);
	EXPECT_EQ(generation.prompt_tps, 0);
	panel.live = generation;
	EXPECT_EQ(AiMetrics::panel_lines(panel)[1], "Live: 40.0 tok/s (gen)");
}

TEST(ai_metrics, live_rate_sums_stable_slots_and_warms_up_on_membership_change) {
	AiMetrics::LiveTracker tracker;
	const auto start = std::chrono::steady_clock::time_point{};
	AiMetrics::SlotSnapshot sample;
	sample.available = true;
	sample.slots = {{0, 41, 100, 0, true}, {1, 42, 200, 0, true}};
	tracker.update(sample, start);
	sample.slots[0].decoded += 40;
	sample.slots[1].decoded += 60;
	EXPECT_EQ(tracker.update(sample, start + 2s).tps, 50);
	sample.slots.pop_back();
	EXPECT_FALSE(tracker.update(sample, start + 4s).tps);
	sample.slots[0].decoded += 60;
	EXPECT_EQ(tracker.update(sample, start + 6s).tps, 30);
}

TEST(ai_metrics, live_summary_keeps_live_and_accumulated_rates_distinct) {
	AiMetrics::Snapshot sample;
	sample.endpoint = "127.0.0.1:8000";
	sample.available = true;
	sample.values.generation_tps = 999;
	sample.values.queued = 1;
	sample.live.enabled = sample.live.available = true;
	sample.live.tps = 40.5;
	sample.live.output_tokens = 180;
	sample.live.active_slots = 2;
	const auto text = AiMetrics::summary(sample);
	EXPECT_NE(text.find("live 40.5 tok/s (gen) | out 180 | active 2 | queue 1"), text.npos);
	EXPECT_EQ(text.find("999"), text.npos);
	sample.live.available = false;
	sample.live.tps.reset();
	sample.live.status = "slots unavailable (enable --slots)";
	const auto fallback = AiMetrics::summary(sample);
	EXPECT_NE(fallback.find("live N/A: slots unavailable"), fallback.npos);
	EXPECT_NE(fallback.find("gen(avg) 999.0"), fallback.npos);
	sample.live.enabled = false;
	EXPECT_EQ(AiMetrics::summary(sample).find("live"), std::string::npos);
}

TEST(ai_metrics, inference_panel_reserves_space_only_when_the_list_still_fits) {
	EXPECT_EQ(AiMetrics::panel_rows(80, 30, true), 7);
	EXPECT_EQ(AiMetrics::panel_rows(44, 17, true), 7);
	EXPECT_EQ(AiMetrics::panel_rows(44, 16, true), 7);
	EXPECT_EQ(AiMetrics::panel_rows(44, 14, true), 7);
	EXPECT_EQ(AiMetrics::panel_rows(44, 13, true), 0);
	EXPECT_EQ(AiMetrics::panel_rows(43, 30, true), 0);
	EXPECT_EQ(AiMetrics::panel_rows(80, 30, false), 0);
}

TEST(ai_metrics, inference_panel_separates_live_counts_and_server_average) {
	AiMetrics::Snapshot sample;
	sample.endpoint = "127.0.0.1:8000";
	sample.available = true;
	sample.values.active = 1;
	sample.values.queued = 0;
	sample.values.generated_tokens = 7470;
	sample.values.generation_seconds = 227.973;
	sample.live.enabled = sample.live.available = true;
	sample.live.tps = 38;
	sample.live.output_tokens = 125;
	sample.live.prompt_processed = 20;
	sample.live.active_slots = 1;
	const auto lines = AiMetrics::panel_lines(sample);
	ASSERT_EQ(lines.size(), 5);
	EXPECT_EQ(lines[0], "Server: 127.0.0.1:8000");
	EXPECT_EQ(lines[1], "Live: 38.0 tok/s (gen)");
	EXPECT_EQ(lines[2], "Active: 1 | Queue: 0");
	EXPECT_EQ(lines[3], "Output: 125 | Prompt: 20");
	EXPECT_EQ(lines[4], "Avg(total): 32.8 tok/s");
	for (const auto& line : lines) EXPECT_LE(line.size(), 40);
	sample.live.tps.reset();
	EXPECT_EQ(AiMetrics::panel_lines(sample)[1], "Live: sampling (needs two readings)");
	sample.live.available = false;
	sample.live.status = "slots unavailable (enable --slots)";
	const auto fallback = AiMetrics::panel_lines(sample);
	EXPECT_EQ(fallback[1], "Avg(total): 32.8 tok/s");
	EXPECT_EQ(fallback[3], "Total output: 7470");
	EXPECT_EQ(fallback[4], "Live: slots unavailable (enable --slots)");
	sample.available = false;
	sample.status = "authentication required";
	const auto failed = AiMetrics::panel_lines(sample);
	EXPECT_EQ(failed[1], "Avg(total): N/A tok/s");
	EXPECT_EQ(failed[2], "Active: N/A | Queue: N/A");
	EXPECT_EQ(failed[3], "Total output: N/A");
	EXPECT_EQ(failed[4], "Status: authentication required");
}

TEST(ai_metrics, parses_named_connections_and_per_server_settings) {
	const auto connections = AiMetrics::parse_connections(R"([
		{"name":"Primary","endpoint":"localhost:8000","api_key_file":"/private/alpha.key","pid":1234},
		{"name":"Worker","endpoint":"127.0.0.1:8001","live":true}
	])", false);
	ASSERT_TRUE(connections);
	ASSERT_EQ(connections->size(), 2);
	EXPECT_EQ(connections->at(0).endpoint, "127.0.0.1:8000");
	EXPECT_EQ(connections->at(0).key_file, "/private/alpha.key");
	EXPECT_FALSE(connections->at(0).live);
	EXPECT_EQ(connections->at(0).process_pid, 1234);
	const auto automatic = AiMetrics::parse_connections(R"([{"endpoint":"localhost:9000","pid":"auto"}])");
	ASSERT_TRUE(automatic);
	EXPECT_TRUE(automatic->front().process_auto);
	EXPECT_EQ(automatic->front().process_pid, 0);
	EXPECT_TRUE(connections->at(1).live);
	EXPECT_TRUE(connections->at(1).key_file.empty()); // never inherits another server's key
	EXPECT_TRUE(AiMetrics::parse_connections("[]"));
	EXPECT_TRUE(Config::stringValid("ai_servers_file", "/private/servers.json"));
	EXPECT_TRUE(Config::stringValid("ai_servers_file", ""));
	EXPECT_FALSE(Config::stringValid("ai_servers_file", "~/servers.json"));
}

TEST(ai_metrics, rejects_ambiguous_or_unsafe_connections) {
	for (const auto body : {"{}", "[", "[{}]", R"([{"endpoint":"remote.example:8000"}])",
		R"([{"endpoint":"127.0.0.1:0"}])",
		R"([{"endpoint":"localhost:8000"},{"endpoint":"127.0.0.1:8000"}])",
		R"([{"name":"same","endpoint":"localhost:8000"},{"name":"same","endpoint":"localhost:8001"}])",
		R"([{"endpoint":"localhost:8000","api_key":"inline-secret"}])",
		R"([{"endpoint":"localhost:8000","api_key_file":"relative/key"}])",
		R"([{"endpoint":"localhost:8000","pid":0}])",
		R"([{"endpoint":"localhost:8000","pid":-1}])",
		R"([{"endpoint":"localhost:8000","pid":"123"}])",
		R"([{"endpoint":"localhost:8000","pid":"discover"}])",
		R"([{"endpoint":"localhost:8000","api_key_file":"/private/key\u0000suffix"}])",
		R"([{"endpoint":"localhost:8000","name":"\u001b[2J"}])",
		R"([{"endpoint":"localhost:8000","live":"false"}])"})
		EXPECT_FALSE(AiMetrics::parse_connections(body)) << body;
	std::string many = "[";
	for (int i = 0; i < 9; ++i) many += (i == 0 ? "" : ",") + std::string("{\"endpoint\":\"localhost:") + std::to_string(8000+i) + "\"}";
	many += "]";
	EXPECT_FALSE(AiMetrics::parse_connections(many));
	EXPECT_FALSE(AiMetrics::parse_connections(std::string(65537, ' ')));
	EXPECT_FALSE(AiMetrics::parse_connections(std::string(33, '[')+std::string(33, ']')));
}

TEST(ai_metrics, parses_passive_ollama_model_and_vram_telemetry) {
	const auto sample = AiMetrics::parse_ollama(R"({"models":[
		{"name":"gemma:latest","size_vram":5368709120,"digest":"private-but-ignored"},
		{"model":"embed:latest","size_vram":268435456}
	],"unknown":"ignored"})");
	ASSERT_TRUE(sample);
	EXPECT_EQ(sample->runtime, "ollama");
	EXPECT_EQ(sample->loaded_models, 2);
	EXPECT_EQ(sample->model_vram, 5637144576ULL);
	EXPECT_EQ(sample->model_name, "gemma:latest");
	EXPECT_NE(AiMetrics::summary(*sample).find("tokens N/A (no passive API)"), std::string::npos);
	for (const auto body : {"{}", R"({"models":{}})", R"({"models":[1]})",
		R"({"models":[{"name":"bad\u001bname","size_vram":1}]})",
		R"({"models":[{"name":"x","size_vram":-1}]})"})
		EXPECT_FALSE(AiMetrics::parse_ollama(body)) << body;
}

TEST(ai_metrics, explicitly_mapped_process_aggregates_workload_gpu_usage) {
	const std::vector<AiMetrics::ProcessSample> processes{
		{100, 100, 4.5, 1024},
		{101, 100, 25.5, 2048},
		{102, 100, 10.0, 4096},
		{200, 200, 80.0, 8192},
	};
	const auto usage = AiMetrics::aggregate_processes(101, processes);
	EXPECT_TRUE(usage.configured);
	EXPECT_TRUE(usage.available);
	EXPECT_EQ(usage.configured_pid, 101);
	EXPECT_EQ(usage.root_pid, 100);
	EXPECT_EQ(usage.process_count, 3);
	EXPECT_DOUBLE_EQ(usage.gpu_percent, 40.0);
	EXPECT_EQ(usage.gpu_memory, 7168);
	AiMetrics::Snapshot panel;
	panel.runtime = "ollama";
	panel.available = true;
	panel.process = usage;
	const auto process_line = AiMetrics::panel_lines(panel)[4];
	EXPECT_LE(process_line.size(), 38);
	EXPECT_NE(process_line.find("PID 101>100 3p | GPU 40% | VRAM 7K"), std::string::npos);
	const auto missing = AiMetrics::aggregate_processes(999, processes);
	EXPECT_TRUE(missing.configured);
	EXPECT_FALSE(missing.available);
	EXPECT_FALSE(AiMetrics::aggregate_processes(0, processes).configured);
	const auto waiting = AiMetrics::aggregate_processes(0, processes, true);
	EXPECT_TRUE(waiting.configured);
	EXPECT_TRUE(waiting.automatic);
	EXPECT_FALSE(waiting.available);
	AiMetrics::Snapshot auto_panel;
	auto_panel.runtime = "ollama";
	auto_panel.available = true;
	auto_panel.process = waiting;
	EXPECT_EQ(AiMetrics::panel_lines(auto_panel)[4], "PID auto: listener not found");
}

TEST(ai_metrics, multiple_server_labels_identify_the_selected_source) {
	AiMetrics::Snapshot sample;
	sample.endpoint = "127.0.0.1:8001";
	sample.server_name = "Worker";
	sample.server_index = 1;
	sample.server_count = 3;
	sample.available = true;
	sample.values.generation_tps = 42;
	EXPECT_NE(AiMetrics::summary(sample).find("llama.cpp [2/3] Worker 127.0.0.1:8001"), std::string::npos);
	EXPECT_EQ(AiMetrics::panel_lines(sample)[0], "Server: 127.0.0.1:8001 | Worker");
}

TEST(ai_metrics, history_retains_bounded_actual_polls_and_preserves_unknowns) {
	AiMetrics::History history;
	AiMetrics::Snapshot sample;
	sample.available = true;
	sample.values.generation_tps = 999; // an exporter average is not a live sample
	sample.values.queued = 2;
	history.append(sample);
	EXPECT_FALSE(history.values().back().tps);
	EXPECT_EQ(history.values().back().queued, 2);
	sample.live.enabled = sample.live.available = true;
	sample.live.tps = 0;
	history.append(sample);
	EXPECT_EQ(history.values().back().tps, 0);
	sample.available = sample.live.available = false;
	history.append(sample);
	EXPECT_FALSE(history.values().back().tps);
	EXPECT_FALSE(history.values().back().queued);
	sample.available = sample.live.available = true;
	for (int i = 1; i <= 150; ++i) {
		sample.live.tps = i;
		history.append(sample);
	}
	ASSERT_EQ(history.values().size(), 120);
	EXPECT_EQ(history.values().front().tps, 31);
	EXPECT_EQ(history.values().back().tps, 150);
}

TEST(ai_metrics, history_chart_marks_gaps_and_zero_separately_and_supports_tty) {
	const std::deque<AiMetrics::HistoryPoint> points{{10.0, 1.0}, {20.0, 4.0}, {}, {0.0, 0.0}};
	const auto chart = AiMetrics::history_chart(points, 20);
	ASSERT_EQ(chart.size(), 5);
	EXPECT_EQ(chart[0], "tok/s peak 20.0");
	EXPECT_EQ(chart[3], "Queue peak 4.0");
	EXPECT_EQ(chart[1], std::string(17, ' ') + "█  ");
	EXPECT_EQ(chart[2], std::string(16, ' ') + "██?.");
	EXPECT_EQ(chart[4], std::string(16, ' ') + "▂█?.");
	const auto tty = AiMetrics::history_chart(points, 20, true);
	for (const auto& row : tty) {
		EXPECT_LE(row.size(), 20);
		EXPECT_TRUE(std::all_of(row.begin(), row.end(), [](unsigned char c) { return c < 128; }));
	}
	EXPECT_EQ(tty[2], std::string(16, ' ') + "@@?.");
	EXPECT_TRUE(AiMetrics::history_chart(points, 0).empty());
	EXPECT_EQ(AiMetrics::history_chart({}, 20)[0], "tok/s: no samples");
}

TEST(ai_metrics, history_chart_scales_only_the_visible_window) {
	const std::deque<AiMetrics::HistoryPoint> points{{10000.0, 100.0}, {10.0, 1.0}, {20.0, 4.0}};
	const auto chart = AiMetrics::history_chart(points, 2);
	EXPECT_EQ(chart[1], " █");
	EXPECT_EQ(chart[2], "██");
	EXPECT_EQ(chart[4], "▂█");
}

#ifdef __linux__
namespace {
class HttpFixture {
	int listener = -1;
	std::jthread server;
public:
	std::string endpoint;
	std::atomic<int> requests = 0;
	std::atomic<int> authorized = 0;
	std::atomic<int> slot_requests = 0;
	HttpFixture(std::string response, std::chrono::milliseconds delay = 0ms, std::string expected_key = {},
		std::string slot_response = "HTTP/1.0 501 Not Implemented\r\n\r\n", bool advance_slots = false) {
		listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (listener < 0) throw std::runtime_error("test socket unavailable");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = 0;
		if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 or listen(listener, 1) < 0)
			throw std::runtime_error("test bind failed");
		socklen_t length = sizeof(address);
		if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) < 0)
			throw std::runtime_error("test getsockname failed");
		endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
		server = std::jthread([this, response, delay, expected_key, slot_response, advance_slots](std::stop_token stop) {
			while (not stop.stop_requested()) {
				pollfd fd{listener, POLLIN, 0};
				if (poll(&fd, 1, 20) <= 0) continue;
				const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
				if (client < 0) continue;
				++requests;
				pollfd input{client, POLLIN, 0};
				if (poll(&input, 1, 500) > 0) {
					char buffer[512];
					const auto n = recv(client, buffer, sizeof(buffer), 0);
					if (n > 0) {
						std::this_thread::sleep_for(delay);
						const std::string_view request(buffer, n);
						const bool slots = request.starts_with("GET /slots HTTP/1.0");
						if (slots) ++slot_requests;
						if (expected_key.empty() or request.contains("Authorization: Bearer " + expected_key + "\r\n")) {
							++authorized;
							auto body = slots ? slot_response : response;
							if (slots and advance_slots) body = "HTTP/1.0 200 OK\r\n\r\n[{\"id\":0,\"id_task\":42,\"is_processing\":true,"
								"\"next_token\":{\"n_decoded\":" + std::to_string(100 * slot_requests.load()) + "}}]";
							send(client, body.data(), body.size(), MSG_NOSIGNAL);
						} else {
							const std::string denied = "HTTP/1.0 401 Unauthorized\r\n\r\n";
							send(client, denied.data(), denied.size(), MSG_NOSIGNAL);
						}
					}
				}
				close(client);
			}
		});
	}
	~HttpFixture() {
		server.request_stop();
		server.join();
		close(listener);
	}
};
std::string http(const std::string& body) {
	return "HTTP/1.0 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
}

TEST(ai_metrics, fetches_vllm_without_slots) {
	HttpFixture fixture(http("vllm:generation_tokens_total{model_name=\"test\"} 250\n"
		"vllm:num_requests_running 2\nvllm:num_requests_waiting 1\n"));
	const auto sample = AiMetrics::fetch_vllm(fixture.endpoint);
	ASSERT_TRUE(sample.available) << sample.status;
	EXPECT_EQ(sample.runtime, "vllm");
	EXPECT_EQ(sample.values.generated_tokens, 250);
	EXPECT_EQ(sample.values.active, 2);
	EXPECT_EQ(fixture.slot_requests.load(), 0);
	HttpFixture missing("HTTP/1.0 404 Not Found\r\n\r\n");
	EXPECT_EQ(AiMetrics::fetch_vllm(missing.endpoint).status.find("--metrics"), std::string::npos);
}

TEST(ai_metrics, fetches_ollama_ps_without_creating_inference_requests) {
	HttpFixture fixture(http(R"({"models":[{"name":"llama3:latest","size_vram":4294967296}]})"));
	const auto sample = AiMetrics::fetch_ollama(fixture.endpoint);
	ASSERT_TRUE(sample.available) << sample.status;
	EXPECT_EQ(sample.runtime, "ollama");
	EXPECT_EQ(sample.loaded_models, 1);
	EXPECT_EQ(sample.model_vram, 4294967296ULL);
	EXPECT_EQ(sample.model_name, "llama3:latest");
	EXPECT_EQ(fixture.requests.load(), 1);
	EXPECT_EQ(fixture.slot_requests.load(), 0);
	const auto lines = AiMetrics::panel_lines(sample);
	EXPECT_EQ(lines[1], "Loaded models: 1 | llama3:latest");
	EXPECT_EQ(lines[3], "Tokens: N/A (Ollama has no passive API)");
	HttpFixture missing("HTTP/1.0 404 Not Found\r\n\r\n");
	EXPECT_EQ(AiMetrics::fetch_ollama(missing.endpoint).status, "Ollama /api/ps unavailable");
}

TEST(ai_metrics, discovers_the_exact_process_owning_a_listening_port) {
	HttpFixture fixture(http("llamacpp:requests_processing 0\n"));
	const std::vector<AiMetrics::ProcessSample> processes{{static_cast<size_t>(getpid()), static_cast<size_t>(getpid()), 0, 0}};
	EXPECT_EQ(AiMetrics::discover_listener_pid(fixture.endpoint, processes), static_cast<size_t>(getpid()));
	EXPECT_FALSE(AiMetrics::discover_listener_pid("127.0.0.1:1", processes));
	EXPECT_FALSE(AiMetrics::discover_listener_pid(fixture.endpoint, {}));
}

TEST(ai_metrics, fetches_real_loopback_http) {
	HttpFixture fixture("HTTP/1.0 200 OK\r\nProcess-Start-Time-Unix: 123456\r\n\r\n"
		"llamacpp:predicted_tokens_seconds 42.5\nllamacpp:requests_processing 2\nllamacpp:requests_deferred 1\n");
	const auto sample = AiMetrics::fetch_llama(fixture.endpoint);
	ASSERT_TRUE(sample.available) << sample.status;
	EXPECT_EQ(sample.values.generation_tps, 42.5);
	EXPECT_EQ(sample.values.active, 2);
	EXPECT_EQ(sample.values.queued, 1);
	EXPECT_EQ(sample.server_instance, 123456);
	EXPECT_EQ(fixture.requests.load(), 1);
}

TEST(ai_metrics, http_errors_clear_values_and_hide_response_body) {
	HttpFixture auth("HTTP/1.0 401 Unauthorized\r\n\r\nprivate response body");
	const auto sample = AiMetrics::fetch_llama(auth.endpoint);
	EXPECT_FALSE(sample.available);
	EXPECT_FALSE(sample.values.active);
	EXPECT_EQ(sample.status, "authentication required");
	EXPECT_EQ(AiMetrics::summary(sample).find("private"), std::string::npos);
	HttpFixture missing("HTTP/1.0 404 Not Found\r\n\r\n");
	EXPECT_EQ(AiMetrics::fetch_llama(missing.endpoint).status, "HTTP 404 (enable --metrics)");
	HttpFixture disabled("HTTP/1.0 501 Not Implemented\r\n\r\n");
	EXPECT_EQ(AiMetrics::fetch_llama(disabled.endpoint).status, "metrics disabled (enable --metrics)");
	HttpFixture truncated("HTTP/1.0 200 OK\r\nContent-Length: 1000\r\n\r\nllamacpp:requests_processing 1\n");
	EXPECT_EQ(AiMetrics::fetch_llama(truncated.endpoint).status, "incomplete HTTP response");
}

TEST(ai_metrics, slow_server_has_one_total_deadline) {
	HttpFixture slow(http("llamacpp:requests_processing 1\n"), 200ms);
	const auto start = std::chrono::steady_clock::now();
	const auto sample = AiMetrics::fetch_llama(slow.endpoint, 40ms);
	EXPECT_FALSE(sample.available);
	EXPECT_EQ(sample.status, "timeout");
	EXPECT_LT(std::chrono::steady_clock::now() - start, 180ms);
}

TEST(ai_metrics, response_size_is_bounded) {
	HttpFixture huge(http(std::string(300 * 1024, 'x')));
	const auto sample = AiMetrics::fetch_llama(huge.endpoint);
	EXPECT_FALSE(sample.available);
	EXPECT_EQ(sample.status, "response too large");
}

TEST(ai_metrics, background_polling_is_opt_in_and_can_be_disabled) {
	HttpFixture fixture(http("llamacpp:requests_processing 2\n"));
	AiMetrics::configure("");
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ(fixture.requests.load(), 0);
	const auto start = std::chrono::steady_clock::now();
	AiMetrics::configure(fixture.endpoint);
	EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().available; ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_TRUE(AiMetrics::snapshot().available);
	EXPECT_EQ(AiMetrics::snapshot().values.active, 2);
	AiMetrics::configure("");
	EXPECT_FALSE(AiMetrics::snapshot().available);
	EXPECT_EQ(AiMetrics::snapshot().status, "disabled");
	AiMetrics::shutdown();
}

TEST(ai_metrics, changing_endpoint_discards_inflight_results) {
	HttpFixture slow(http("llamacpp:requests_processing 99\n"), 150ms);
	HttpFixture next(http("llamacpp:requests_processing 3\n"));
	AiMetrics::configure(slow.endpoint);
	for (int i = 0; i < 100 and slow.requests.load() == 0; ++i)
		std::this_thread::sleep_for(5ms);
	ASSERT_GT(slow.requests.load(), 0);
	AiMetrics::configure(next.endpoint);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().available; ++i)
		std::this_thread::sleep_for(10ms);
	const auto sample = AiMetrics::snapshot();
	EXPECT_TRUE(sample.available);
	EXPECT_EQ(sample.endpoint, next.endpoint);
	EXPECT_EQ(sample.values.active, 3);
	AiMetrics::configure("");
	AiMetrics::shutdown();
}

namespace {
class KeyFile {
public:
	std::string path;
	explicit KeyFile(const std::string& value) {
		char name[] = "/tmp/btop-test-api-key-XXXXXX";
		const int fd = mkstemp(name);
		if (fd < 0) throw std::runtime_error("temporary key file failed");
		path = name;
		close(fd);
		replace(value);
	}
	void replace(const std::string& value) {
		const int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
		if (fd < 0) throw std::runtime_error("temporary key file open failed");
		const auto count = write(fd, value.data(), value.size());
		close(fd);
		if (count != static_cast<ssize_t>(value.size())) throw std::runtime_error("temporary key write failed");
	}
	~KeyFile() { unlink(path.c_str()); }
};
}

TEST(ai_metrics, api_key_file_authenticates_without_exposing_key) {
	KeyFile key("synthetic-test-key\n");
	HttpFixture fixture(http("llamacpp:requests_processing 2\n"), 0ms, "synthetic-test-key");
	const auto sample = AiMetrics::fetch_llama(fixture.endpoint, 250ms, key.path);
	EXPECT_TRUE(sample.available) << sample.status;
	EXPECT_EQ(fixture.authorized.load(), 1);
	EXPECT_EQ(AiMetrics::summary(sample).find("synthetic-test-key"), std::string::npos);
	EXPECT_EQ(AiMetrics::summary(sample).find(key.path), std::string::npos);
	const auto unauthenticated = AiMetrics::fetch_llama(fixture.endpoint);
	EXPECT_EQ(unauthenticated.status, "authentication required");
}

TEST(ai_metrics, rejects_bad_key_files_without_sending_a_request) {
	HttpFixture fixture(http("llamacpp:requests_processing 2\n"));
	KeyFile key("");
	EXPECT_EQ(AiMetrics::fetch_llama(fixture.endpoint, 250ms, key.path).status, "API key file empty or invalid");
	key.replace("safe\r\nInjected-Header: value");
	EXPECT_EQ(AiMetrics::fetch_llama(fixture.endpoint, 250ms, key.path).status, "API key file empty or invalid");
	key.replace(std::string(4097, 'x'));
	EXPECT_EQ(AiMetrics::fetch_llama(fixture.endpoint, 250ms, key.path).status, "API key file too large");
	EXPECT_EQ(AiMetrics::fetch_llama(fixture.endpoint, 250ms, key.path + ".missing").status, "API key file unreadable");
	EXPECT_EQ(AiMetrics::fetch_llama(fixture.endpoint, 250ms, "/tmp").status, "API key file must be regular");
	EXPECT_EQ(fixture.requests.load(), 0);
}

TEST(ai_metrics, api_key_rotation_is_read_on_next_fetch) {
	KeyFile key("old-synthetic-key\n");
	HttpFixture fixture(http("llamacpp:requests_processing 2\n"), 0ms, "new-synthetic-key");
	EXPECT_EQ(AiMetrics::fetch_llama(fixture.endpoint, 250ms, key.path).status, "authentication required");
	key.replace("new-synthetic-key\r\n");
	EXPECT_TRUE(AiMetrics::fetch_llama(fixture.endpoint, 250ms, key.path).available);
	EXPECT_EQ(fixture.authorized.load(), 1);
}

TEST(ai_metrics, key_file_change_discards_inflight_samples) {
	KeyFile valid("synthetic-test-key\n"), invalid("");
	HttpFixture slow(http("llamacpp:requests_processing 99\n"), 150ms, "synthetic-test-key");
	AiMetrics::configure(slow.endpoint, valid.path);
	for (int i = 0; i < 100 and slow.requests.load() == 0; ++i) std::this_thread::sleep_for(5ms);
	ASSERT_GT(slow.requests.load(), 0);
	AiMetrics::configure(slow.endpoint, invalid.path);
	for (int i = 0; i < 100 and AiMetrics::snapshot().status != "API key file empty or invalid"; ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_EQ(AiMetrics::snapshot().status, "API key file empty or invalid");
	EXPECT_FALSE(AiMetrics::snapshot().available);
	AiMetrics::configure("");
	AiMetrics::shutdown();
}

TEST(ai_metrics, slot_http_uses_key_and_handles_disabled_and_malformed_endpoints) {
	KeyFile key("slot-synthetic-key\n");
	HttpFixture fixture(http("llamacpp:requests_processing 1\n"), 0ms, "slot-synthetic-key",
		http(R"([{"id":0,"id_task":42,"is_processing":true,"next_token":{"n_decoded":99}}])"));
	const auto sample = AiMetrics::fetch_slots(fixture.endpoint, 250ms, key.path);
	ASSERT_TRUE(sample.available) << sample.status;
	ASSERT_EQ(sample.slots.size(), 1);
	EXPECT_EQ(sample.slots[0].decoded, 99);
	EXPECT_EQ(fixture.slot_requests.load(), 1);
	EXPECT_EQ(fixture.authorized.load(), 1);
	EXPECT_EQ(AiMetrics::fetch_slots(fixture.endpoint).status, "authentication required");
	HttpFixture unsupported(http("llamacpp:requests_processing 1\n"));
	EXPECT_EQ(AiMetrics::fetch_slots(unsupported.endpoint).status, "slots unavailable (enable --slots)");
	HttpFixture invalid(http("llamacpp:requests_processing 1\n"), 0ms, {}, http("private invalid JSON"));
	const auto bad = AiMetrics::fetch_slots(invalid.endpoint);
	EXPECT_EQ(bad.status, "unsupported slot data");
	EXPECT_TRUE(bad.slots.empty());
}

TEST(ai_metrics, background_live_toggle_resets_state_and_stops_slot_requests) {
	HttpFixture fixture(http("llamacpp:requests_processing 1\n"), 0ms, {},
		http(R"([{"id":0,"id_task":42,"is_processing":true,"next_token":{"n_decoded":99}}])"));
	AiMetrics::configure(fixture.endpoint, "", true);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().live.available; ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_TRUE(AiMetrics::snapshot().live.available);
	EXPECT_EQ(AiMetrics::snapshot().live.output_tokens, 99);
	EXPECT_FALSE(AiMetrics::snapshot().live.tps);
	AiMetrics::configure(fixture.endpoint, "", false);
	EXPECT_FALSE(AiMetrics::snapshot().live.enabled);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().available; ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_TRUE(AiMetrics::snapshot().available);
	EXPECT_EQ(fixture.slot_requests.load(), 1);
	AiMetrics::configure(fixture.endpoint, "", true);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().live.available; ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_FALSE(AiMetrics::snapshot().live.tps); // no baseline survives toggling
	AiMetrics::configure("");
	EXPECT_FALSE(AiMetrics::snapshot().live.available);
	AiMetrics::shutdown();
}

TEST(ai_metrics, unavailable_slots_preserve_metrics_and_explain_fallback) {
	HttpFixture fixture(http("llamacpp:predicted_tokens_seconds 42.5\n"));
	AiMetrics::configure(fixture.endpoint);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().available; ++i)
		std::this_thread::sleep_for(10ms);
	const auto sample = AiMetrics::snapshot();
	EXPECT_TRUE(sample.available);
	EXPECT_FALSE(sample.live.available);
	EXPECT_EQ(sample.live.status, "slots unavailable (enable --slots)");
	EXPECT_NE(AiMetrics::summary(sample).find("gen(avg) 42.5"), std::string::npos);
	AiMetrics::configure("");
	AiMetrics::shutdown();
}

TEST(ai_metrics, background_poller_calculates_live_rate_from_slot_http) {
	HttpFixture fixture(http("llamacpp:requests_processing 1\n"), 0ms, {}, "", true);
	AiMetrics::configure(fixture.endpoint);
	for (int i = 0; i < 400 and not AiMetrics::snapshot().live.tps; ++i)
		std::this_thread::sleep_for(10ms);
	const auto sample = AiMetrics::snapshot();
	EXPECT_TRUE(sample.live.available);
	ASSERT_TRUE(sample.live.tps);
	EXPECT_GT(*sample.live.tps, 30);
	EXPECT_LT(*sample.live.tps, 60);
	EXPECT_GE(sample.live.output_tokens, 200);
	EXPECT_FALSE(sample.live.prompt_processed); // field absent: never invent zero
	AiMetrics::configure("");
	AiMetrics::shutdown();
}

TEST(ai_metrics, sglang_fetch_and_exported_rate_toggle) {
	HttpFixture fixture(http("sglang:gen_throughput 123.5\nsglang:num_running_reqs 2\n"
		"sglang:num_queue_reqs 1\nsglang:generation_tokens_total 100\n"), 0ms, "sglang-test-token");
	KeyFile key("sglang-test-token");
	const auto fetched = AiMetrics::fetch_sglang(fixture.endpoint, 250ms, key.path);
	ASSERT_TRUE(fetched.available) << fetched.status;
	EXPECT_EQ(fetched.runtime, "sglang");
	EXPECT_EQ(fetched.values.generation_tps, 123.5);
	EXPECT_EQ(fetched.values.active, 2);
	const auto entry = "[{\"runtime\":\"sglang\",\"endpoint\":\"" + fixture.endpoint
		+ "\",\"api_key_file\":\"" + key.path + "\",\"live\":";
	KeyFile servers(entry + "true}]");
	AiMetrics::configure("", "", true, servers.path);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().available; ++i) std::this_thread::sleep_for(10ms);
	const auto sample = AiMetrics::snapshot();
	EXPECT_EQ(sample.live.tps, 123.5); // no second reading or completed-token delta needed
	EXPECT_NE(AiMetrics::summary(sample).find("gen(exporter) 123.5"), std::string::npos);
	EXPECT_EQ(AiMetrics::panel_lines(sample)[1], "Gen(exporter): 123.5 tok/s");
	ASSERT_FALSE(sample.history.empty());
	EXPECT_EQ(sample.history.back().tps, 123.5);
	servers.replace(entry + "false}]");
	for (int i = 0; i < 400 and AiMetrics::snapshot().live.enabled; ++i) std::this_thread::sleep_for(10ms);
	const auto disabled = AiMetrics::snapshot();
	EXPECT_TRUE(disabled.available);
	EXPECT_FALSE(disabled.live.enabled);
	EXPECT_FALSE(disabled.live.tps);
	ASSERT_FALSE(disabled.history.empty());
	EXPECT_FALSE(disabled.history.back().tps);
	EXPECT_EQ(fixture.slot_requests.load(), 0);
	EXPECT_GE(fixture.authorized.load(), 3);
	AiMetrics::configure("");
	AiMetrics::shutdown();
}

TEST(ai_metrics, sglang_missing_throughput_does_not_invent_a_rate) {
	HttpFixture fixture(http("sglang:generation_tokens_total 500\nsglang:num_running_reqs 1\n"));
	KeyFile servers("[{\"runtime\":\"sglang\",\"endpoint\":\"" + fixture.endpoint + "\"}]");
	AiMetrics::configure("", "", true, servers.path);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().available; ++i) std::this_thread::sleep_for(10ms);
	const auto sample = AiMetrics::snapshot();
	EXPECT_TRUE(sample.available);
	EXPECT_FALSE(sample.live.available);
	EXPECT_FALSE(sample.live.tps);
	EXPECT_EQ(sample.live.status, "throughput gauge unavailable");
	EXPECT_EQ(AiMetrics::panel_lines(sample)[1], "Gen(exporter): N/A");
	EXPECT_EQ(sample.values.generated_tokens, 500);
	ASSERT_FALSE(sample.history.empty());
	EXPECT_FALSE(sample.history.back().tps);
	EXPECT_EQ(fixture.slot_requests.load(), 0);
	AiMetrics::configure("");
	AiMetrics::shutdown();
	HttpFixture missing("HTTP/1.0 404 Not Found\r\n\r\n");
	EXPECT_EQ(AiMetrics::fetch_sglang(missing.endpoint).status, "metrics unavailable (enable --enable-metrics)");
}

TEST(ai_metrics, connections_poll_independently_and_switch_without_resetting_rates) {
	KeyFile alpha_key("alpha-synthetic"), beta_key("beta-synthetic");
	HttpFixture alpha(http("llamacpp:requests_processing 1\n"), 0ms, "alpha-synthetic",
		http(R"([{"id":0,"id_task":42,"is_processing":true,"next_token":{"n_decoded":99}}])"));
	HttpFixture beta(http("llamacpp:requests_processing 2\n"), 0ms, "beta-synthetic", "", true);
	const auto alpha_json = "{\"name\":\"Alpha\",\"endpoint\":\"" + alpha.endpoint + "\",\"api_key_file\":\"" + alpha_key.path + "\"}";
	const auto beta_json = "{\"name\":\"Beta\",\"endpoint\":\"" + beta.endpoint + "\",\"api_key_file\":\"" + beta_key.path + "\"}";
	KeyFile servers("[" + alpha_json + "," + beta_json + "]");
	AiMetrics::configure("", "", true, servers.path);
	std::vector<AiMetrics::Snapshot> values;
	for (int i = 0; i < 500; ++i) {
		values = AiMetrics::snapshots();
		if (values.size() == 2 and values[0].live.tps and values[1].live.tps) break;
		std::this_thread::sleep_for(10ms);
	}
	ASSERT_EQ(values.size(), 2);
	EXPECT_EQ(values[0].values.active, 1);
	EXPECT_EQ(values[1].values.active, 2);
	EXPECT_EQ(values[0].live.tps, 0);
	ASSERT_TRUE(values[1].live.tps);
	EXPECT_GT(*values[1].live.tps, 30);
	EXPECT_EQ(values[0].live.output_tokens, 99);
	EXPECT_GE(values[1].live.output_tokens, 200);
	EXPECT_GE(alpha.authorized.load(), 4);
	EXPECT_GE(beta.authorized.load(), 4);
	EXPECT_GE(values[0].history.size(), 2);
	EXPECT_GE(values[1].history.size(), 2);
	EXPECT_EQ(values[0].history.back().tps, 0);
	ASSERT_TRUE(values[1].history.back().tps);
	EXPECT_GT(*values[1].history.back().tps, 30);
	AiMetrics::cycle_server(1);
	EXPECT_EQ(AiMetrics::snapshot().server_name, "Beta");
	EXPECT_TRUE(AiMetrics::snapshot().live.tps); // switching doesn't reset its history
	AiMetrics::cycle_server(1);
	EXPECT_EQ(AiMetrics::snapshot().server_name, "Alpha");
	AiMetrics::cycle_server(-1);
	EXPECT_EQ(AiMetrics::snapshot().server_name, "Beta");
	servers.replace("[" + beta_json + "," + alpha_json + "]");
	for (int i = 0; i < 400 and AiMetrics::snapshot().server_index != 0; ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_EQ(AiMetrics::snapshot().server_name, "Beta");
	EXPECT_EQ(AiMetrics::snapshot().server_index, 0);
	EXPECT_TRUE(AiMetrics::snapshot().live.tps); // reordering preserves history
	EXPECT_GE(AiMetrics::snapshot().history.size(), values[1].history.size());
	AiMetrics::configure("");
	AiMetrics::shutdown();
}

TEST(ai_metrics, runtime_reload_resets_history_and_live_setting) {
	HttpFixture fixture(http("vllm:generation_tokens_total 100\nvllm:num_requests_running 2\n"
		"llamacpp:requests_processing 1\n"), 0ms, "", http("[]"));
	auto entry = [&](const std::string& runtime, bool live) {
		return "[{\"endpoint\":\"" + fixture.endpoint + "\",\"runtime\":\"" + runtime
			+ "\",\"live\":" + (live ? "true" : "false") + "}]";
	};
	KeyFile servers(entry("vllm", true));
	AiMetrics::configure("", "", true, servers.path);
	auto wait = [](auto predicate) {
		for (int i = 0; i < 500; ++i) {
			if (predicate(AiMetrics::snapshot())) return true;
			std::this_thread::sleep_for(10ms);
		}
		return false;
	};
	EXPECT_TRUE(wait([](const auto& s) { return s.runtime == "vllm" and s.live.tps.has_value(); }));
	EXPECT_EQ(fixture.slot_requests.load(), 0);
	servers.replace(entry("llama.cpp", true));
	EXPECT_TRUE(wait([](const auto& s) { return s.runtime == "llama.cpp" and s.available; }));
	EXPECT_EQ(AiMetrics::snapshot().values.active, 1);
	EXPECT_LE(AiMetrics::snapshot().history.size(), 1);
	EXPECT_GT(fixture.slot_requests.load(), 0);
	servers.replace(entry("vllm", false));
	EXPECT_TRUE(wait([](const auto& s) { return s.runtime == "vllm" and s.available; }));
	EXPECT_FALSE(AiMetrics::snapshot().live.enabled);
	EXPECT_LE(AiMetrics::snapshot().history.size(), 1);
	EXPECT_EQ(AiMetrics::snapshot().values.active, 2);
	AiMetrics::configure("");
	AiMetrics::shutdown();
}

TEST(ai_metrics, slow_server_does_not_block_healthy_connections) {
	HttpFixture slow(http("llamacpp:requests_processing 99\n"), 400ms);
	HttpFixture healthy(http("llamacpp:requests_processing 3\n"));
	KeyFile servers("[{\"name\":\"Slow\",\"endpoint\":\"" + slow.endpoint
		+ "\"},{\"name\":\"Healthy\",\"endpoint\":\"" + healthy.endpoint + "\",\"live\":false}]");
	AiMetrics::configure("", "", true, servers.path);
	bool ready = false;
	for (int i = 0; i < 80; ++i) {
		const auto values = AiMetrics::snapshots();
		if (values.size() == 2 and values[1].available) { ready = true; break; }
		std::this_thread::sleep_for(5ms);
	}
	EXPECT_TRUE(ready); // healthy responds before the slow request's deadline
	AiMetrics::cycle_server(1);
	EXPECT_EQ(AiMetrics::snapshot().values.active, 3);
	EXPECT_FALSE(AiMetrics::snapshot().live.enabled);
	AiMetrics::configure("");
	AiMetrics::shutdown();
}

TEST(ai_metrics, invalid_connections_file_clears_sources_without_using_legacy_credentials) {
	HttpFixture legacy(http("llamacpp:requests_processing 9\n"));
	HttpFixture source(http("llamacpp:requests_processing 1\n"));
	KeyFile servers("[{\"endpoint\":\"" + source.endpoint + "\",\"live\":false}]");
	AiMetrics::configure(legacy.endpoint, "/private/unused-key", true, servers.path);
	for (int i = 0; i < 100 and not AiMetrics::snapshot().available; ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_EQ(AiMetrics::snapshot().values.active, 1);
	EXPECT_EQ(legacy.requests.load(), 0);
	servers.replace("[{\"api_key\":\"do-not-display-this\"}]");
	for (int i = 0; i < 400 and not AiMetrics::snapshots().empty(); ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_TRUE(AiMetrics::snapshots().empty());
	EXPECT_FALSE(AiMetrics::snapshot().available);
	EXPECT_EQ(AiMetrics::snapshot().status, "invalid connections file (see guide)");
	EXPECT_EQ(AiMetrics::summary(AiMetrics::snapshot()).find("do-not-display-this"), std::string::npos);
	EXPECT_EQ(legacy.requests.load(), 0);
	servers.replace("[{\"endpoint\":\"" + source.endpoint + "\",\"live\":false}]");
	for (int i = 0; i < 400 and not AiMetrics::snapshot().available; ++i)
		std::this_thread::sleep_for(10ms);
	EXPECT_EQ(AiMetrics::snapshot().values.active, 1);
	AiMetrics::configure("");
	AiMetrics::shutdown();
}
#endif

TEST(ai_metrics, vllm_labeled_metrics_and_validation) {
	const auto sample = AiMetrics::parse_vllm(
		"vllm:generation_tokens_total{model_name=\"test}model\",engine=\"0\"} 100\n"
		"vllm:generation_tokens_total{engine=\"1\",model_name=\"test}model\"} 200\n"
		"vllm:prompt_tokens_total 600\n"
		"vllm:num_requests_running{engine=\"0\"} 2\n"
		"vllm:num_requests_running{engine=\"1\"} 3\n"
		"vllm:num_requests_waiting 0\nprocess_start_time_seconds 123.5\n");
	ASSERT_TRUE(sample);
	EXPECT_EQ(sample->generated_tokens, 300);
	EXPECT_EQ(sample->active, 5);
	EXPECT_EQ(sample->queued, 0);
	EXPECT_EQ(sample->prompt_tokens, 600);
	EXPECT_EQ(sample->process_start, 123.5);
	EXPECT_EQ(sample->generation_series.size(), 2);
	for (const auto& body : {
		"vllm:generation_tokens_total NaN\n", "vllm:num_requests_running -1\n",
		"vllm:generation_tokens_total 1.5\n", "process_start_time_seconds 12\n",
		"vllm:generation_tokens_total{bad} 1\n",
		"vllm:generation_tokens_total{a=\"b\",a=\"c\"} 1\n",
		"vllm:generation_tokens_total{a=\"b\",c=\"d\"} 1\nvllm:generation_tokens_total{c=\"d\",a=\"b\"} 2\n"})
		EXPECT_FALSE(AiMetrics::parse_vllm(body)) << body;
}

TEST(ai_metrics, vllm_counter_rate_resets_and_series_membership) {
	AiMetrics::Snapshot sample;
	sample.runtime = "vllm";
	sample.available = true;
	sample.values.generation_series = {{"engine0",100}, {"engine1",200}};
	AiMetrics::CounterTracker tracker;
	const auto start = std::chrono::steady_clock::time_point{};
	EXPECT_FALSE(tracker.update(sample, start).tps);
	sample.values.generation_series = {{"engine0",120}, {"engine1",260}};
	EXPECT_EQ(tracker.update(sample, start + 2s).tps, 40);
	EXPECT_EQ(tracker.update(sample, start + 4s).tps, 0);
	// One reset hidden by another engine's growth must invalidate the rate.
	sample.values.generation_series = {{"engine0",1}, {"engine1",1000}};
	EXPECT_FALSE(tracker.update(sample, start + 6s).tps);
	sample.values.generation_series = {{"engine2",1}, {"engine1",1000}};
	EXPECT_FALSE(tracker.update(sample, start + 8s).tps);
	sample.available = false;
	EXPECT_FALSE(tracker.update(sample, start + 10s).available);
	sample.available = true;
	EXPECT_FALSE(tracker.update(sample, start + 12s).tps);
	sample.values.process_start = 123.5;
	EXPECT_FALSE(tracker.update(sample, start + 14s).tps);
	EXPECT_EQ(tracker.update(sample, start + 16s).tps, 0);
	EXPECT_FALSE(tracker.update(sample, start + 30s).tps);
	sample.server_instance = 42;
	EXPECT_FALSE(tracker.update(sample, start + 32s).tps);
	sample.values.generation_series.clear();
	EXPECT_FALSE(tracker.update(sample, start + 34s).available);
}

TEST(ai_metrics, vllm_configuration_and_display) {
	const auto entries = AiMetrics::parse_connections(R"([{"endpoint":"localhost:8000","runtime":"vllm"},{"endpoint":"127.0.0.1:8001"}])");
	ASSERT_TRUE(entries);
	EXPECT_EQ((*entries)[0].runtime, "vllm");
	EXPECT_EQ((*entries)[1].runtime, "llama.cpp");
	EXPECT_FALSE(AiMetrics::parse_connections(R"([{"endpoint":"localhost:8000","runtime":"unknown"}])"));
	EXPECT_FALSE(AiMetrics::parse_connections(R"([{"endpoint":"localhost:8000","runtime":123}])"));
	AiMetrics::Snapshot sample;
	sample.runtime = "vllm";
	sample.available = true;
	sample.live.enabled = sample.live.available = true;
	sample.live.tps = 42;
	sample.values.active = 3;
	sample.values.generated_tokens = 500;
	sample.values.prompt_tokens = 600;
	const auto lines = AiMetrics::panel_lines(sample);
	EXPECT_EQ(lines[1], "Rate: 42.0 tok/s");
	EXPECT_EQ(lines[2], "Active: 3 | Queue: N/A");
	EXPECT_EQ(lines[3], "Total out: 500 | Prompt: 600");
	EXPECT_NE(AiMetrics::summary(sample).find("vLLM | rate 42.0"), std::string::npos);
	AiMetrics::History history;
	history.append(sample);
	EXPECT_EQ(history.values().back().tps, 42);
}

TEST(ai_metrics, parses_sglang_exporter_gauges_and_totals) {
	const auto values = AiMetrics::parse_sglang(
		"sglang:gen_throughput{model_name=\"test\",dp_rank=\"0\"} 42.5\n"
		"sglang:gen_throughput{dp_rank=\"1\",model_name=\"test\"} 10.25\n"
		"sglang:num_running_reqs 2\nsglang:num_queue_reqs 0\n"
		"sglang:generation_tokens_total{is_streaming=\"true\"} 100\n"
		"sglang:generation_tokens_total{is_streaming=\"false\"} 200\n"
		"sglang:prompt_tokens_total 400\n");
	ASSERT_TRUE(values);
	EXPECT_EQ(values->generation_tps, 52.75);
	EXPECT_EQ(values->generated_tokens, 300);
	EXPECT_EQ(values->prompt_tokens, 400);
	EXPECT_EQ(values->active, 2);
	EXPECT_EQ(values->queued, 0);
	for (const auto& body : {"sglang:gen_throughput NaN\n", "sglang:gen_throughput -1\n",
		"sglang:num_queue_reqs 0.5\n", "sglang:gen_throughput 1\nsglang:gen_throughput{} 2\n",
		"sglang:gen_throughput{a=\"b\",c=\"d\"} 1\nsglang:gen_throughput{c=\"d\",a=\"b\"} 2\n",
		"vllm:generation_tokens_total 100\n"})
		EXPECT_FALSE(AiMetrics::parse_sglang(body)) << body;
	const auto zero = AiMetrics::parse_sglang("sglang:gen_throughput 0\n");
	ASSERT_TRUE(zero);
	EXPECT_EQ(zero->generation_tps, 0);
	EXPECT_FALSE(zero->generated_tokens);
	EXPECT_FALSE(AiMetrics::parse_vllm("sglang:gen_throughput 1\n"));
}

TEST(ai_metrics, sglang_connection_runtime_and_display) {
	const auto entries = AiMetrics::parse_connections(R"([{"endpoint":"localhost:30000","runtime":"sglang"}])");
	ASSERT_TRUE(entries);
	EXPECT_EQ(entries->front().runtime, "sglang");
	AiMetrics::Snapshot sample;
	sample.runtime = "sglang";
	sample.available = true;
	sample.live.enabled = sample.live.available = true;
	sample.live.tps = 0;
	sample.values.generation_tps = 0;
	const auto lines = AiMetrics::panel_lines(sample);
	EXPECT_TRUE(lines[0].starts_with("SGLang:"));
	EXPECT_EQ(lines[1], "Gen(exporter): 0.0 tok/s");
	EXPECT_EQ(lines[2], "Active: N/A | Queue: N/A");
	EXPECT_EQ(lines[4], "Rate from exporter window");
	AiMetrics::History history;
	history.append(sample);
	EXPECT_EQ(history.values().back().tps, 0);
	sample.available = false;
	sample.values = {};
	sample.live.available = false;
	sample.live.tps.reset();
	history.append(sample);
	EXPECT_FALSE(history.values().back().tps);
	EXPECT_FALSE(history.values().back().queued);
}
