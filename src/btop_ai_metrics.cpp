// SPDX-License-Identifier: Apache-2.0
#include "btop_ai_metrics.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <limits>
#include <set>
#include <memory>

#ifdef __linux__
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace AiMetrics {
using namespace std::chrono_literals;

auto loopback_port(std::string_view endpoint) -> std::optional<uint16_t> {
	if (endpoint.starts_with("127.0.0.1:")) endpoint.remove_prefix(10);
	else if (endpoint.starts_with("localhost:")) endpoint.remove_prefix(10);
	else return std::nullopt;
	unsigned port{};
	const auto [end, error] = std::from_chars(endpoint.data(), endpoint.data() + endpoint.size(), port);
	if (error != std::errc{} or end != endpoint.data() + endpoint.size() or port == 0 or port > 65535)
		return std::nullopt;
	return static_cast<uint16_t>(port);
}

auto parse_prometheus(std::string_view body) -> std::optional<Values> {
	Values values;
	bool found = false;
	while (not body.empty()) {
		const auto newline = body.find('\n');
		auto line = body.substr(0, newline);
		body.remove_prefix(newline == body.npos ? body.size() : newline + 1);
		if (line.empty() or line.front() == '#') continue;
		const auto name_end = line.find_first_of("{ \t\r");
		const auto name = line.substr(0, name_end);
		std::optional<double>* target = nullptr;
		if (name == "llamacpp:predicted_tokens_seconds") target = &values.generation_tps;
		else if (name == "llamacpp:tokens_predicted_total") target = &values.generated_tokens;
		else if (name == "llamacpp:tokens_predicted_seconds_total") target = &values.generation_seconds;
		else if (name == "llamacpp:requests_processing") target = &values.active;
		else if (name == "llamacpp:requests_deferred") target = &values.queued;
		else continue;
		// Multiple labeled series require explicit aggregation semantics, especially for averages.
		if (target->has_value() or name_end == line.npos) return std::nullopt;
		line.remove_prefix(name_end);
		if (line.starts_with('{')) {
			bool quoted = false, escaped = false;
			size_t pos = 1;
			for (; pos < line.size(); ++pos) {
				const char c = line[pos];
				if (escaped) { escaped = false; continue; }
				if (quoted and c == '\\') { escaped = true; continue; }
				if (c == '"') quoted = not quoted;
				if (c == '}' and not quoted) break;
			}
			if (pos == line.size()) return std::nullopt;
			line.remove_prefix(pos + 1);
		}
		if (line.empty() or (line.front() != ' ' and line.front() != '\t')) return std::nullopt;
		const auto value_start = line.find_first_not_of(" \t");
		if (value_start == line.npos) return std::nullopt;
		line.remove_prefix(value_start);
		double value{};
		const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), value);
		if (error != std::errc{} or not std::isfinite(value) or value < 0
			or (end != line.data() + line.size() and *end != ' ' and *end != '\t' and *end != '\r'))
			return std::nullopt;
		if (target != &values.generation_tps and target != &values.generation_seconds
			and std::floor(value) != value) return std::nullopt;
		*target = value;
		found = true;
	}
	return found ? std::optional(values) : std::nullopt;
}

// Canonical label order prevents reordered labels from disguising duplicates.
namespace {
auto series_labels(std::string_view line) -> std::optional<std::string> {
	if (not line.starts_with('{')) return std::string{};
	line.remove_prefix(1);
	std::map<std::string, std::string> labels;
	auto spaces = [&] { while (not line.empty() and (line.front() == ' ' or line.front() == '\t')) line.remove_prefix(1); };
	while (true) {
		spaces();
		if (line.starts_with('}')) break;
		const auto end = line.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
		if (end == 0 or end == line.npos or std::isdigit(static_cast<unsigned char>(line.front()))) return std::nullopt;
		std::string name(line.substr(0, end));
		line.remove_prefix(end);
		spaces();
		if (not line.starts_with('=')) return std::nullopt;
		line.remove_prefix(1);
		spaces();
		if (not line.starts_with('"')) return std::nullopt;
		line.remove_prefix(1);
		std::string value;
		bool closed = false;
		while (not line.empty()) {
			char c = line.front(); line.remove_prefix(1);
			if (c == '"') { closed = true; break; }
			if (c == '\\') {
				if (line.empty()) return std::nullopt;
				c = line.front(); line.remove_prefix(1);
				if (c != '\\' and c != '"' and c != 'n') return std::nullopt;
				if (c == 'n') c = '\n';
			}
			value += c;
		}
		if (not closed or not labels.emplace(name, value).second) return std::nullopt;
		spaces();
		if (line.starts_with('}')) break;
		if (not line.starts_with(',')) return std::nullopt;
		line.remove_prefix(1);
	}
	std::string identity;
	for (const auto& [key, value] : labels)
		identity += std::to_string(key.size()) + ":" + key + std::to_string(value.size()) + ":" + value;
	return identity;
}
}

namespace {
auto parse_exporter(std::string_view body, bool sglang) -> std::optional<Values> {
	if (body.size() > 256 * 1024) return std::nullopt;
	Values result;
	std::set<std::pair<std::string, std::string>> seen;
	bool found = false;
	while (not body.empty()) {
		const auto newline = body.find('\n');
		const auto line = body.substr(0, newline);
		body.remove_prefix(newline == body.npos ? body.size() : newline + 1);
		const auto end = line.find_first_of("{ \t\r");
		const auto name = line.substr(0, end);
		std::optional<double>* target = nullptr;
		if (name == (sglang ? "sglang:generation_tokens_total" : "vllm:generation_tokens_total")) target = &result.generated_tokens;
		else if (name == (sglang ? "sglang:prompt_tokens_total" : "vllm:prompt_tokens_total")) target = &result.prompt_tokens;
		else if (name == (sglang ? "sglang:num_running_reqs" : "vllm:num_requests_running")) target = &result.active;
		else if (name == (sglang ? "sglang:num_queue_reqs" : "vllm:num_requests_waiting")) target = &result.queued;
		else if (sglang and name == "sglang:gen_throughput") target = &result.generation_tps;
		else if (name == "process_start_time_seconds") target = &result.process_start;
		else continue;
		if (end == line.npos) return std::nullopt;
		const auto identity = series_labels(line.substr(end));
		if (not identity or not seen.emplace(std::string(name), *identity).second) return std::nullopt;
		// Reuse the numeric/label scanner. Timestamps and exported throughput
		// allow fractions; token totals and request counts must be integers.
		const bool fractional = target == &result.process_start or target == &result.generation_tps;
		const auto parsed = parse_prometheus(std::string(fractional
			? "llamacpp:tokens_predicted_seconds_total" : "llamacpp:tokens_predicted_total") + std::string(line.substr(end)));
		if (not parsed) return std::nullopt;
		const double value = fractional ? *parsed->generation_seconds : *parsed->generated_tokens;
		if (target == &result.process_start) {
			if (target->has_value()) return std::nullopt;
			*target = value;
		}
		else {
			const double total = target->value_or(0) + value;
			if (not std::isfinite(total)) return std::nullopt;
			*target = total;
			found = true;
		}
		if (target == &result.generated_tokens) result.generation_series.emplace(*identity, value);
	}
	return found ? std::optional(result) : std::nullopt;
}

} // namespace

auto parse_vllm(std::string_view body) -> std::optional<Values> { return parse_exporter(body, false); }
auto parse_sglang(std::string_view body) -> std::optional<Values> { return parse_exporter(body, true); }

namespace {
auto runtime_name(std::string_view runtime) -> std::string {
	if (runtime == "vllm") return "vLLM";
	if (runtime == "sglang") return "SGLang";
	return "llama.cpp";
}
// SGLang's throughput gauge describes the exporter's window. Its completed
// request token counter cannot establish the speed of an in-flight request.
auto exported_rate(const Snapshot& sample) -> LiveValues {
	LiveValues result;
	result.enabled = true;
	result.status = sample.status;
	if (not sample.available) return result;
	result.available = sample.values.generation_tps.has_value();
	result.tps = sample.values.generation_tps;
	result.status = result.available ? "exporter" : "throughput gauge unavailable";
	return result;
}
} // namespace

auto CounterTracker::update(const Snapshot& sample, std::chrono::steady_clock::time_point now) -> LiveValues {
	LiveValues result;
	result.enabled = true;
	result.status = sample.status;
	if (not sample.available or sample.values.generation_series.empty()) {
		primed = false;
		if (sample.available) result.status = "generation counter unavailable";
		return result;
	}
	result.available = true;
	result.status = "sampling";
	const auto seconds = std::chrono::duration<double>(now - previous_time).count();
	bool comparable = primed and seconds > 0 and seconds <= 10
		and sample.server_instance == previous_instance and sample.values.process_start == previous.process_start
		and sample.values.generation_series.size() == previous.generation_series.size();
	double delta = 0;
	for (const auto& [identity, value] : sample.values.generation_series) {
		const auto old = previous.generation_series.find(identity);
		if (old == previous.generation_series.end() or value < old->second) comparable = false;
		else delta += value - old->second;
	}
	if (comparable and std::isfinite(delta / seconds)) {
		result.tps = delta / seconds;
		result.status = "ready";
	}
	previous = sample.values;
	previous_instance = sample.server_instance;
	previous_time = now;
	primed = true;
	return result;
}

#ifdef __linux__
namespace {
struct Socket {
	int fd = -1;
	~Socket() { if (fd >= 0) close(fd); }
};
auto wait_socket(int fd, short events, std::chrono::steady_clock::time_point deadline) -> bool {
	while (std::chrono::steady_clock::now() < deadline) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		pollfd descriptor{fd, events, 0};
		const int result = poll(&descriptor, 1, static_cast<int>(std::max(1LL, static_cast<long long>(left.count()))));
		if (result > 0) return descriptor.revents & (events | POLLHUP | POLLERR);
		if (result < 0 and errno == EINTR) continue;
		return false;
	}
	return false;
}
}
#endif

namespace {
struct HttpResult {
	std::string status;
	std::string body;
	bool available = false;
	std::optional<uint64_t> server_instance;
};
auto fetch_http(std::string_view endpoint, std::string_view path, std::chrono::milliseconds budget, std::string_view key_file) -> HttpResult {
	HttpResult result;
	const auto port = loopback_port(endpoint);
	if (not port) { result.status = "invalid endpoint (use 127.0.0.1:port)"; return result; }
#ifdef __linux__
	// Read only the configured file, on the polling thread; reload each scrape for key rotation.
	std::string key;
	if (not key_file.empty()) {
		Socket file{open(std::string(key_file).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
		if (file.fd < 0) { result.status = "API key file unreadable"; return result; }
		struct stat info{};
		if (fstat(file.fd, &info) < 0 or not S_ISREG(info.st_mode)) {
			result.status = "API key file must be regular"; return result;
		}
		char bytes[4097];
		while (key.size() <= 4096) {
			const auto count = read(file.fd, bytes, sizeof(bytes) - key.size());
			if (count < 0 and errno == EINTR) continue;
			if (count < 0) { result.status = "API key file unreadable"; return result; }
			if (count == 0) break;
			key.append(bytes, count);
		}
		if (key.size() > 4096) { result.status = "API key file too large"; return result; }
		while (not key.empty() and (key.back() == '\n' or key.back() == '\r')) key.pop_back();
		if (key.empty() or key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~+/=") != key.npos) {
			result.status = "API key file empty or invalid"; return result;
		}
	}
	// One network deadline bounds connect, write and read. No DNS or redirects.
	const auto deadline = std::chrono::steady_clock::now() + budget;
	Socket socket{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
	if (socket.fd < 0) { result.status = "socket unavailable"; return result; }
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(*port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
		if (errno != EINPROGRESS) { result.status = "unreachable"; return result; }
		if (not wait_socket(socket.fd, POLLOUT, deadline)) { result.status = "timeout"; return result; }
		int error{};
		socklen_t length = sizeof(error);
		if (getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 or error != 0) {
			result.status = "unreachable"; return result;
		}
	}
	const std::string request = "GET " + std::string(path) + " HTTP/1.0\r\nHost: " + std::string(endpoint)
		+ "\r\nAccept: text/plain\r\n"
		+ (key.empty() ? std::string{} : "Authorization: Bearer " + key + "\r\n")
		+ "Connection: close\r\n\r\n";
	size_t sent = 0;
	while (sent < request.size()) {
		if (not wait_socket(socket.fd, POLLOUT, deadline)) { result.status = "timeout"; return result; }
		const auto count = send(socket.fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
		if (count < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)) continue;
		if (count <= 0) { result.status = "connection closed"; return result; }
		sent += static_cast<size_t>(count);
	}
	std::string response;
	constexpr size_t limit = 256 * 1024;
	while (true) {
		if (not wait_socket(socket.fd, POLLIN, deadline)) { result.status = "timeout"; return result; }
		char buffer[8192];
		const auto count = recv(socket.fd, buffer, sizeof(buffer), 0);
		if (count == 0) break;
		if (count < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)) continue;
		if (count < 0) { result.status = "read failed"; return result; }
		if (response.size() + static_cast<size_t>(count) > limit) { result.status = "response too large"; return result; }
		response.append(buffer, count);
	}
	const auto header_end = response.find("\r\n\r\n");
	if (header_end == response.npos or (not response.starts_with("HTTP/1.0 ") and not response.starts_with("HTTP/1.1 "))) {
		result.status = "invalid HTTP response"; return result;
	}
	unsigned code{};
	const auto [end, error] = std::from_chars(response.data() + 9, response.data() + 12, code);
	if (error != std::errc{} or end != response.data() + 12 or response[12] != ' ') {
		result.status = "invalid HTTP status"; return result;
	}
	if (code != 200) {
		result.status = code == 401 or code == 403 ? "authentication required"
			: (code == 404 or code == 501) and path == "/slots" ? "slots unavailable (enable --slots)"
			: code == 404 ? "HTTP 404 (enable --metrics)"
			: code == 501 ? "metrics disabled (enable --metrics)" : "HTTP " + std::to_string(code);
		return result;
	}
	auto headers = response.substr(0, header_end);
	std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (headers.contains("transfer-encoding:") or headers.contains("content-encoding:")) {
		result.status = "unsupported HTTP encoding"; return result;
	}
	auto body = std::string_view(response).substr(header_end + 4);
	if (const auto at = headers.find("\r\nprocess-start-time-unix:"); at != headers.npos) {
		auto value = std::string_view(headers).substr(at + 26);
		const auto start = value.find_first_not_of(" \t");
		if (start != value.npos) {
			value.remove_prefix(start);
			uint64_t instance{};
			const auto [last, err] = std::from_chars(value.data(), value.data() + value.size(), instance);
			if (err == std::errc{} and (last == value.data() + value.size() or *last == '\r'))
				result.server_instance = instance;
		}
	}
	if (const auto at = headers.find("\r\ncontent-length:"); at != headers.npos) {
		auto length = std::string_view(headers).substr(at + 17);
		const auto start = length.find_first_not_of(" \t");
		if (start == length.npos) { result.status = "invalid content length"; return result; }
		length.remove_prefix(start);
		size_t count{};
		const auto [last, err] = std::from_chars(length.data(), length.data() + length.size(), count);
		if (err != std::errc{} or (last != length.data() + length.size() and *last != '\r') or count != body.size()) {
			result.status = "incomplete HTTP response"; return result;
		}
	}
	result.body = body;
	result.available = true;
	result.status = "ready";
#else
	(void)budget;
	(void)key_file;
	(void)path;
	result.status = "adapter requires Linux";
#endif
	return result;
}
} // namespace

auto fetch_llama(std::string_view endpoint, std::chrono::milliseconds budget, std::string_view key_file) -> Snapshot {
	Snapshot result;
	if (loopback_port(endpoint)) result.endpoint = endpoint;
	auto response = fetch_http(endpoint, "/metrics", budget, key_file);
	result.status = response.status;
	if (not response.available) return result;
	const auto values = parse_prometheus(response.body);
	if (not values) { result.status = "unrecognized metrics"; return result; }
	result.values = *values;
	result.server_instance = response.server_instance;
	result.available = true;
	return result;
}

namespace {
auto fetch_exporter(std::string_view endpoint, std::chrono::milliseconds budget, std::string_view key_file, const std::string& runtime) -> Snapshot {
	Snapshot result;
	result.runtime = runtime;
	if (loopback_port(endpoint)) result.endpoint = endpoint;
	const auto response = fetch_http(endpoint, "/metrics", budget, key_file);
	result.status = response.status;
	if (not response.available) {
		if (result.status.find("--metrics") != std::string::npos) result.status = runtime == "sglang" ? "metrics unavailable (enable --enable-metrics)" : "metrics unavailable (check /metrics)";
		return result;
	}
	const auto values = runtime == "sglang" ? parse_sglang(response.body) : parse_vllm(response.body);
	if (not values) { result.status = "unrecognized " + runtime_name(runtime) + " metrics"; return result; }
	result.values = *values;
	result.server_instance = response.server_instance;
	result.available = true;
	return result;
}

} // namespace

auto fetch_vllm(std::string_view endpoint, std::chrono::milliseconds budget, std::string_view key_file) -> Snapshot {
	return fetch_exporter(endpoint, budget, key_file, "vllm");
}
auto fetch_sglang(std::string_view endpoint, std::chrono::milliseconds budget, std::string_view key_file) -> Snapshot {
	return fetch_exporter(endpoint, budget, key_file, "sglang");
}

auto fetch_slots(std::string_view endpoint, std::chrono::milliseconds budget, std::string_view key_file) -> SlotSnapshot {
	SlotSnapshot result;
	auto response = fetch_http(endpoint, "/slots", budget, key_file);
	result.status = response.status;
	if (not response.available) return result;
	const auto slots = parse_slots(response.body);
	if (not slots) { result.status = "unsupported slot data"; return result; }
	result.slots = *slots;
	result.available = true;
	return result;
}

namespace {
auto bounded_json(std::string_view body, size_t limit) -> bool {
	if (body.size() > limit) return false;
	int depth = 0;
	bool quoted = false, escaped = false;
	for (const char c : body) {
		if (escaped) { escaped = false; continue; }
		if (quoted and c == '\\') { escaped = true; continue; }
		if (c == '"') quoted = not quoted;
		if (quoted) continue;
		if ((c == '[' or c == '{') and ++depth > 32) return false;
		if ((c == ']' or c == '}') and --depth < 0) return false;
	}
	return depth == 0 and not quoted;
}
}

auto parse_slots(std::string_view body) -> std::optional<std::vector<Slot>> {
	// Bound nesting before building a DOM, including unknown fields. No prompts
	// or generated text survive parsing, even when a server includes them.
	if (not bounded_json(body, 256 * 1024)) return std::nullopt;
	const auto data = nlohmann::json::parse(body, nullptr, false);
	if (not data.is_array() or data.size() > 256) return std::nullopt;
	auto number = [](const nlohmann::json& object, const char* key) -> std::optional<uint64_t> {
		const auto it = object.find(key);
		if (it == object.end() or not it->is_number_integer()) return std::nullopt;
		if (it->is_number_unsigned()) return it->get<uint64_t>();
		const auto value = it->get<int64_t>();
		if (value < 0) return std::nullopt;
		return static_cast<uint64_t>(value);
	};
	std::vector<Slot> slots;
	std::set<uint64_t> ids;
	for (const auto& entry : data) {
		if (not entry.is_object()) return std::nullopt;
		const auto id = number(entry, "id");
		const auto processing = entry.find("is_processing");
		if (not id or not ids.insert(*id).second or processing == entry.end() or not processing->is_boolean())
			return std::nullopt;
		Slot slot;
		slot.id = *id;
		slot.processing = processing->get<bool>();
		if (slot.processing) {
			const auto task = number(entry, "id_task");
			auto next = entry.find("next_token");
			if (not task or next == entry.end()) return std::nullopt;
			// llama.cpp has shipped both an object and a one-element array.
			if (next->is_array()) {
				if (next->size() != 1) return std::nullopt;
				next = next->begin();
			}
			if (not next->is_object()) return std::nullopt;
			const auto decoded = number(*next, "n_decoded");
			if (not decoded) return std::nullopt;
			slot.task = *task;
			slot.decoded = *decoded;
			slot.prompt_processed = number(entry, "n_prompt_tokens_processed");
			if (entry.contains("n_prompt_tokens_processed") and not slot.prompt_processed) return std::nullopt;
		}
		slots.push_back(slot);
	}
	return slots;
}

auto parse_connections(std::string_view body, bool default_live) -> std::optional<std::vector<Connection>> {
	if (not bounded_json(body, 64 * 1024)) return std::nullopt;
	const auto data = nlohmann::json::parse(body, nullptr, false);
	if (not data.is_array() or data.size() > 8) return std::nullopt;
	std::vector<Connection> connections;
	std::set<uint16_t> ports;
	std::set<std::string> names;
	for (const auto& item : data) {
		if (not item.is_object()) return std::nullopt;
		// Reject typos, especially misplaced inline credentials.
		for (const auto& key : item.items())
			if (key.key() != "name" and key.key() != "endpoint" and key.key() != "api_key_file" and key.key() != "live" and key.key() != "runtime")
				return std::nullopt;
		if (not item.contains("endpoint") or not item["endpoint"].is_string()) return std::nullopt;
		Connection connection;
		connection.live = default_live;
		if (item.contains("runtime")) {
			if (not item["runtime"].is_string()) return std::nullopt;
			connection.runtime = item["runtime"].get<std::string>();
			if (connection.runtime != "llama.cpp" and connection.runtime != "vllm" and connection.runtime != "sglang") return std::nullopt;
		}
		const auto port = loopback_port(item["endpoint"].get<std::string>());
		if (not port or not ports.insert(*port).second) return std::nullopt;
		connection.endpoint = "127.0.0.1:" + std::to_string(*port);
		if (item.contains("name")) {
			if (not item["name"].is_string()) return std::nullopt;
			connection.name = item["name"].get<std::string>();
			if (connection.name.empty() or connection.name.size() > 24 or not names.insert(connection.name).second
				or std::any_of(connection.name.begin(), connection.name.end(), [](unsigned char c) { return c < 32 or c > 126; }))
				return std::nullopt;
		}
		if (item.contains("api_key_file")) {
			if (not item["api_key_file"].is_string()) return std::nullopt;
			connection.key_file = item["api_key_file"].get<std::string>();
			if (not connection.key_file.empty() and (connection.key_file.front() != '/' or connection.key_file.size() > 4096
				or std::any_of(connection.key_file.begin(), connection.key_file.end(), [](unsigned char c) { return c < 32 or c == 127; })))
				return std::nullopt;
		}
		if (item.contains("live")) {
			if (not item["live"].is_boolean()) return std::nullopt;
			connection.live = item["live"].get<bool>();
		}
		connections.push_back(std::move(connection));
	}
	return connections;
}

auto LiveTracker::update(const SlotSnapshot& sample, std::chrono::steady_clock::time_point now) -> LiveValues {
	LiveValues result;
	result.enabled = true;
	result.status = sample.status;
	if (not sample.available) { previous.clear(); primed = false; return result; }
	result.available = true;
	const auto seconds = std::chrono::duration<double>(now - previous_time).count();
	bool comparable = primed and seconds > 0 and seconds <= 10 and sample.server_instance == previous_instance;
	double delta = 0;
	size_t active = 0;
	for (const auto& slot : sample.slots) {
		if (not slot.processing) continue;
		++active;
		// Saturating sums prevent malformed remote counters wrapping in the UI.
		result.output_tokens += std::min(slot.decoded, std::numeric_limits<uint64_t>::max() - result.output_tokens);
		if (result.prompt_processed and slot.prompt_processed)
			*result.prompt_processed += std::min(*slot.prompt_processed, std::numeric_limits<uint64_t>::max() - *result.prompt_processed);
		else result.prompt_processed.reset();
		const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& item) {
			return item.processing and item.id == slot.id and item.task == slot.task;
		});
		if (old == previous.end() or slot.decoded < old->decoded) comparable = false;
		else delta += static_cast<double>(slot.decoded - old->decoded);
	}
	// A departed task makes this interval incomplete. Warm up instead of
	// reporting an undercount, or a spike from a reused slot's new counter.
	for (const auto& old : previous) {
		if (old.processing and std::none_of(sample.slots.begin(), sample.slots.end(), [&](const auto& slot) {
			return slot.processing and slot.id == old.id and slot.task == old.task;
		})) comparable = false;
	}
	if (active == 0) { result.tps = 0; result.status = "idle"; }
	else if (comparable) { result.tps = delta / seconds; result.status = "ready"; }
	else result.status = "sampling";
	result.active_slots = active;
	previous = sample.slots;
	previous_time = now;
	previous_instance = sample.server_instance;
	primed = true;
	return result;
}

auto summary(const Snapshot& sample) -> std::string {
	std::string prefix = runtime_name(sample.runtime);
	if (sample.server_count > 1) prefix += " [" + std::to_string(sample.server_index + 1) + "/" + std::to_string(sample.server_count) + "]";
	if (not sample.server_name.empty()) prefix += " " + sample.server_name;
	if (not sample.endpoint.empty()) prefix += " " + sample.endpoint;
	if (sample.runtime == "vllm" or sample.runtime == "sglang") {
		if (not sample.available) return prefix + ": " + sample.status;
		auto count = [](std::optional<double> v) { return v ? fmt::format("{:.0f}", *v) : "N/A"; };
		const auto rate = sample.live.tps ? fmt::format("{:.1f}", *sample.live.tps) : "N/A";
		return prefix + (sample.runtime == "sglang" ? " | gen(exporter) " : " | rate ") + rate + " tok/s | active " + count(sample.values.active)
			+ " | queue " + count(sample.values.queued) + " | total out " + count(sample.values.generated_tokens);
	}
	if (sample.live.enabled and sample.live.available) {
		const auto speed = sample.live.tps ? fmt::format("{:.1f}", *sample.live.tps) : "N/A (sampling)";
		const auto queued = sample.available and sample.values.queued ? fmt::format("{:.0f}", *sample.values.queued) : "N/A";
		return prefix + " | live " + speed + " tok/s | out " + std::to_string(sample.live.output_tokens)
			+ " | active " + std::to_string(sample.live.active_slots) + " | queue " + queued
			+ " | prompt " + (sample.live.prompt_processed ? std::to_string(*sample.live.prompt_processed) : "N/A");
	}
	const auto live_status = sample.live.enabled ? " | live N/A: " + sample.live.status : std::string{};
	if (not sample.available) return prefix + ": " + sample.status + live_status;
	// Some llama.cpp versions reset the throughput gauge between completions.
	// Counter ratios remain useful, but describe accumulated work, not live speed.
	auto rate = sample.values.generation_tps;
	std::string rate_label = "gen(avg)";
	if (not rate or *rate == 0) {
		rate.reset();
		if (sample.values.generated_tokens and sample.values.generation_seconds
			and *sample.values.generated_tokens > 0 and *sample.values.generation_seconds > 0) {
			const auto average = *sample.values.generated_tokens / *sample.values.generation_seconds;
			if (std::isfinite(average)) {
				rate = average;
				rate_label = "gen(total)";
			}
		}
	}
	const auto speed = rate ? fmt::format("{:.1f}", *rate) : "N/A";
	const auto active = sample.values.active ? fmt::format("{:.0f}", *sample.values.active) : "N/A";
	const auto queued = sample.values.queued ? fmt::format("{:.0f}", *sample.values.queued) : "N/A";
	const auto tokens = sample.values.generated_tokens
		? " | tokens " + fmt::format("{:.0f}", *sample.values.generated_tokens) : std::string{};
	return prefix + live_status + " | " + rate_label + " " + speed + " tok/s | active " + active + " | queue " + queued + tokens;
}

void History::append(const Snapshot& sample) {
	HistoryPoint point;
	if (sample.live.enabled and sample.live.available and sample.live.tps
		and std::isfinite(*sample.live.tps) and *sample.live.tps >= 0) point.tps = sample.live.tps;
	if (sample.available and sample.values.queued and std::isfinite(*sample.values.queued)
		and *sample.values.queued >= 0) point.queued = sample.values.queued;
	points.push_back(point);
	if (points.size() > 120) points.pop_front();
}

auto history_chart(const std::deque<HistoryPoint>& points, int width, bool tty) -> std::vector<std::string> {
	if (width < 1) return {};
	std::vector<std::string> rows(5);
	const size_t count = std::min(points.size(), static_cast<size_t>(width));
	const size_t start = points.size() - count;
	double peak = 0, queue_peak = 0;
	for (size_t i = start; i < points.size(); ++i) {
		if (points[i].tps) peak = std::max(peak, *points[i].tps);
		if (points[i].queued) queue_peak = std::max(queue_peak, *points[i].queued);
	}
	auto number = [](double value) { return value < 10000 ? fmt::format("{:.1f}", value) : fmt::format("{:.3g}", value); };
	rows[0] = "tok/s peak " + number(peak);
	rows[3] = "Queue peak " + number(queue_peak);
	const bool has_rate = std::any_of(points.begin() + start, points.end(), [](const auto& p) { return p.tps.has_value(); });
	const bool has_queue = std::any_of(points.begin() + start, points.end(), [](const auto& p) { return p.queued.has_value(); });
	if (not has_rate) rows[0] = "tok/s: no samples";
	if (not has_queue) rows[3] = "Queue: no samples";
	for (const int row : {1, 2, 4}) rows[row] = std::string(width - count, ' ');
	const std::vector<std::string> blocks = tty ? std::vector<std::string>{" ", ".", ":", "-", "=", "+", "*", "#", "@"}
		: std::vector<std::string>{" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
	for (size_t i = start; i < points.size(); ++i) {
		if (not points[i].tps) { rows[1] += ' '; rows[2] += '?'; }
		else if (*points[i].tps == 0) { rows[1] += ' '; rows[2] += '.'; }
		else {
			const int level = std::clamp(static_cast<int>(std::ceil(*points[i].tps / std::max(peak, 1.0) * 16)), 1, 16);
			rows[1] += blocks[std::max(0, level - 8)];
			rows[2] += blocks[std::min(8, level)];
		}
		if (not points[i].queued) rows[4] += '?';
		else if (*points[i].queued == 0) rows[4] += '.';
		else rows[4] += blocks[std::clamp(static_cast<int>(std::ceil(*points[i].queued / std::max(queue_peak, 1.0) * 8)), 1, 8)];
	}
	rows[0].resize(std::min(rows[0].size(), static_cast<size_t>(width)));
	rows[3].resize(std::min(rows[3].size(), static_cast<size_t>(width)));
	return rows;
}

auto panel_rows(int width, int available_height, bool enabled) -> int {
	return enabled and width >= 44 and available_height >= 14 ? 7 : 0;
}

auto panel_lines(const Snapshot& sample) -> std::vector<std::string> {
	auto count = [](std::optional<double> value) -> std::string {
		if (not value) return "N/A";
		return *value < 100000 ? fmt::format("{:.0f}", *value) : fmt::format("{:.3g}", *value);
	};
	if (sample.runtime == "vllm" or sample.runtime == "sglang") {
		const auto rate = sample.live.tps ? fmt::format("{:.1f}", *sample.live.tps) + " tok/s" : "N/A";
		return {runtime_name(sample.runtime) + ": " + sample.endpoint + (sample.server_name.empty() ? "" : " | " + sample.server_name),
			(sample.runtime == "sglang" ? "Gen(exporter): " : "Rate: ") + rate,
			"Active: " + count(sample.values.active) + " | Queue: " + count(sample.values.queued),
			"Total out: " + count(sample.values.generated_tokens) + " | Prompt: " + count(sample.values.prompt_tokens),
			not sample.available ? "Status: " + sample.status : not sample.live.enabled ? "Rate polling off" :
				sample.live.tps ? (sample.runtime == "sglang" ? "Rate from exporter window" : "Rate from exported counter deltas") : "Rate: " + sample.live.status};
	}
	auto average = sample.values.generation_tps;
	std::string label = "Avg(exporter): ";
	if (not average or *average == 0) {
		average.reset();
		if (sample.values.generated_tokens and sample.values.generation_seconds
			and *sample.values.generated_tokens > 0 and *sample.values.generation_seconds > 0) {
			const auto rate = *sample.values.generated_tokens / *sample.values.generation_seconds;
			if (std::isfinite(rate)) average = rate;
		}
		label = "Avg(total): ";
	}
	const auto average_line = label + (sample.available and average ? fmt::format("{:.1f}", *average) : "N/A") + " tok/s";
	const auto queued = sample.available ? count(sample.values.queued) : "N/A";
	const auto active = sample.live.enabled and sample.live.available ? count(static_cast<double>(sample.live.active_slots))
		: sample.available ? count(sample.values.active) : "N/A";
	std::vector<std::string> lines(5);
	lines[0] = "Server: " + sample.endpoint;
	if (not sample.server_name.empty()) lines[0] += " | " + sample.server_name;
	lines[1] = average_line;
	lines[2] = "Active: " + active + " | Queue: " + queued;
	lines[3] = "Total output: " + (sample.available ? count(sample.values.generated_tokens) : "N/A");
	lines[4] = sample.available ? "Live polling off (Options > Processes)" : "Status: " + sample.status;
	if (sample.live.enabled and sample.live.available) {
		lines[1] = sample.live.tps ? "Live: " + fmt::format("{:.1f}", *sample.live.tps) + " tok/s"
			: "Live: sampling (needs two readings)";
		lines[3] = "Output: " + count(static_cast<double>(sample.live.output_tokens)) + " | Prompt: "
			+ (sample.live.prompt_processed ? count(static_cast<double>(*sample.live.prompt_processed)) : "N/A");
		lines[4] = sample.available ? average_line : "Metrics: " + sample.status;
	}
	else if (sample.live.enabled and sample.available) lines[4] = "Live: " + sample.live.status;
	return lines;
}

namespace {
class Poller {
	std::mutex mutex;
	std::condition_variable_any changed;
	std::string endpoint;
	std::string key_file;
	bool live = true;
	LiveTracker tracker;
	CounterTracker counter_tracker;
	std::string runtime = "llama.cpp";
	History history;
	uint64_t revision = 0;
	Snapshot value;
	// Declared last so the worker joins before its synchronization state is destroyed.
	std::jthread worker;
public:
	void configure(const std::string& next, const std::string& next_key_file, bool next_live, const std::string& next_runtime) {
		std::lock_guard lock(mutex);
		if (next == endpoint and next_key_file == key_file and next_live == live and next_runtime == runtime) return;
		endpoint = next;
		key_file = next_key_file;
		live = next_live;
		runtime = next_runtime;
		counter_tracker = {};
		tracker = {};
		history = {};
		++revision;
		value = {};
		value.runtime = runtime;
		if (not endpoint.empty()) {
			if (loopback_port(endpoint)) { value.endpoint = endpoint; value.status = "connecting"; }
			else value.status = "invalid endpoint (use 127.0.0.1:port)";
		}
		if (not worker.joinable() and loopback_port(endpoint)) {
			worker = std::jthread([this](std::stop_token stop) {
				std::unique_lock guard(mutex);
				while (not stop.stop_requested()) {
					changed.wait(guard, stop, [this] { return loopback_port(endpoint).has_value(); });
					if (stop.stop_requested()) break;
					const auto target = endpoint;
					const auto credential_path = key_file;
					const auto collect_live = live;
					const auto provider = runtime;
					const auto version = revision;
					guard.unlock();
					auto result = provider == "llama.cpp" ? fetch_llama(target, 250ms, credential_path)
						: fetch_exporter(target, 250ms, credential_path, provider);
					SlotSnapshot slots;
					if (collect_live and provider == "llama.cpp") slots = fetch_slots(target, 250ms, credential_path);
					const auto sampled_at = std::chrono::steady_clock::now();
					guard.lock();
					if (version == revision) {
						slots.server_instance = result.server_instance;
						if (collect_live) {
							if (provider == "vllm") result.live = counter_tracker.update(result, sampled_at);
							else if (provider == "sglang") result.live = exported_rate(result);
							else result.live = tracker.update(slots, sampled_at);
						}
						history.append(result);
						result.history = history.values();
						value = std::move(result);
					}
					changed.wait_for(guard, stop, 2s, [this, version] { return revision != version; });
				}
			});
		}
		changed.notify_all();
	}
	auto snapshot() -> Snapshot { std::lock_guard lock(mutex); return value; }
	void shutdown() {
		if (worker.joinable()) {
			worker.request_stop();
			changed.notify_all();
			worker.join();
		}
	}
	~Poller() { shutdown(); }
};
struct ConnectionFile {
	std::vector<Connection> connections;
	std::string status;
};
auto load_connections(const std::string& path, bool live) -> ConnectionFile {
#ifdef __linux__
	if (path.empty() or path.front() != '/' or path.size() > 4096)
		return {{}, "connections file needs an absolute path"};
	Socket file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
	if (file.fd < 0) return {{}, "connections file unreadable"};
	struct stat info{};
	if (fstat(file.fd, &info) < 0 or not S_ISREG(info.st_mode)) return {{}, "connections file must be regular"};
	std::string body;
	char bytes[4096];
	while (body.size() <= 64 * 1024) {
		const auto count = read(file.fd, bytes, sizeof(bytes));
		if (count < 0 and errno == EINTR) continue;
		if (count < 0) return {{}, "connections file unreadable"};
		if (count == 0) break;
		body.append(bytes, count);
	}
	if (body.size() > 64 * 1024) return {{}, "connections file too large"};
	const auto connections = parse_connections(body, live);
	if (not connections) return {{}, "invalid connections file (see guide)"};
	return {*connections, connections->empty() ? "no servers configured" : "ready"};
#else
	(void)path;
	(void)live;
	return {{}, "connections file requires Linux"};
#endif
}

class Connections {
	struct Settings {
		std::string endpoint, key_file, file;
		bool live = true;
		bool operator==(const Settings&) const = default;
	};
	struct Entry {
		Connection config;
		std::shared_ptr<Poller> poller;
	};
	std::mutex mutex;
	std::condition_variable_any changed;
	Settings requested;
	std::vector<Entry> entries;
	size_t selected = 0;
	uint64_t revision = 0;
	bool pending = false;
	std::string status = "disabled";
	// Declared last: stop and join the loader before destroying shared state.
	std::jthread loader;

	void run(std::stop_token stop) {
		std::unique_lock lock(mutex);
		while (not stop.stop_requested()) {
			const auto settings = requested;
			const auto version = revision;
			auto previous = entries;
			lock.unlock();
			ConnectionFile file;
			if (not settings.file.empty()) file = load_connections(settings.file, settings.live);
			else if (settings.endpoint.empty()) file.status = "disabled";
			else if (not loopback_port(settings.endpoint)) file.status = "invalid endpoint (use 127.0.0.1:port)";
			else {
				file.connections.push_back({"", settings.endpoint, settings.key_file, settings.live});
				file.status = "ready";
			}
			std::vector<Entry> next;
			for (const auto& connection : file.connections) {
				const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& entry) {
					return entry.config.endpoint == connection.endpoint;
				});
				auto source = old == previous.end() ? std::make_shared<Poller>() : old->poller;
				source->configure(connection.endpoint, connection.key_file, connection.live, connection.runtime);
				next.push_back({connection, std::move(source)});
			}
			lock.lock();
			if (version == revision and not stop.stop_requested()) {
				const auto identity = selected < entries.size() ? entries[selected].config.endpoint : std::string{};
				entries = next;
				selected = 0;
				for (size_t i = 0; i < entries.size(); ++i)
					if (entries[i].config.endpoint == identity) selected = i;
				status = file.status;
				pending = false;
			}
			lock.unlock();
			// Retired workers can take up to their network deadline to join. Do
			// this on the loader thread, without holding the snapshot mutex.
			previous.clear();
			next.clear();
			lock.lock();
			changed.wait_for(lock, stop, 2s, [this, version] { return revision != version; });
		}
	}
public:
	void configure(const std::string& endpoint, const std::string& key, bool live, const std::string& file) {
		std::lock_guard lock(mutex);
		const Settings next{endpoint, key, file, live};
		if (next == requested) return;
		requested = next;
		++revision;
		pending = true;
		status = not file.empty() ? "loading connections file" : endpoint.empty() ? "disabled" : "connecting";
		if (not loader.joinable()) loader = std::jthread([this](std::stop_token stop) { run(stop); });
		changed.notify_all();
	}
	auto snapshot() -> Snapshot {
		std::lock_guard lock(mutex);
		if (pending or entries.empty()) {
			Snapshot result;
			result.status = status;
			if (requested.file.empty() and loopback_port(requested.endpoint)) result.endpoint = requested.endpoint;
			return result;
		}
		auto result = entries[selected].poller->snapshot();
		result.server_name = entries[selected].config.name;
		result.server_index = selected;
		result.server_count = entries.size();
		return result;
	}
	auto snapshots() -> std::vector<Snapshot> {
		std::lock_guard lock(mutex);
		if (pending) return {};
		std::vector<Snapshot> result;
		for (size_t i = 0; i < entries.size(); ++i) {
			auto value = entries[i].poller->snapshot();
			value.server_name = entries[i].config.name;
			value.server_index = i;
			value.server_count = entries.size();
			result.push_back(std::move(value));
		}
		return result;
	}
	void cycle(int direction) {
		std::lock_guard lock(mutex);
		if (pending or entries.empty()) return;
		if (direction > 0) selected = (selected + 1) % entries.size();
		else if (direction < 0) selected = selected == 0 ? entries.size() - 1 : selected - 1;
	}
	void shutdown() {
		if (loader.joinable()) {
			loader.request_stop();
			changed.notify_all();
			loader.join();
		}
		std::vector<Entry> retired;
		{
			std::lock_guard lock(mutex);
			retired.swap(entries);
			requested = {};
			pending = false;
			selected = 0;
			status = "disabled";
			++revision;
		}
		retired.clear();
	}
	~Connections() { shutdown(); }
};
auto connections() -> Connections& { static Connections instance; return instance; }
}
void configure(const std::string& endpoint, const std::string& key_file, bool live, const std::string& servers_file) {
	connections().configure(endpoint, key_file, live, servers_file);
}
auto snapshot() -> Snapshot { return connections().snapshot(); }
auto snapshots() -> std::vector<Snapshot> { return connections().snapshots(); }
void cycle_server(int direction) { connections().cycle(direction); }
void shutdown() { connections().shutdown(); }
} // namespace AiMetrics
