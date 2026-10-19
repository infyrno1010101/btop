// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

// Conservative command-line hints, not proof of model identity or job state.
namespace Workload {
struct Info {
	std::string_view runtime;
	std::string_view role;
};

inline auto next_token(std::string_view& command) -> std::string_view {
	const auto start = command.find_first_not_of(" \t");
	if (start == command.npos) { command = {}; return {}; }
	command.remove_prefix(start);
	const auto end = command.find_first_of(" \t");
	const auto token = command.substr(0, end);
	command.remove_prefix(end == command.npos ? command.size() : end);
	return token;
}

inline auto detect(std::string_view command) -> Info {
	auto executable = next_token(command);
	executable = executable.substr(executable.find_last_of('/') + 1);
	if (executable == "llama-server") return {"llama.cpp", "serving"};
	if (executable == "llama-cli") return {"llama.cpp", "inference"};
	const auto argument = next_token(command);
	if (executable == "ollama" and (argument == "serve" or argument == "runner"))
		return {"Ollama", argument == "serve" ? "serving" : "worker"};
	if (executable == "vllm" and argument == "serve") return {"vLLM", "serving"};
	// torchrun is a distributed launcher: it does not establish training vs inference.
	if (executable == "torchrun") return {"PyTorch", "distributed"};
	const bool python = executable == "python" or executable == "python3"
		or (executable.starts_with("python3.") and executable.size() > 8
			and executable.substr(8).find_first_not_of("0123456789") == executable.npos);
	if (not python or argument != "-m") return {};
	const auto module = next_token(command);
	if (module == "vllm.entrypoints.openai.api_server" or module == "vllm.entrypoints.api_server")
		return {"vLLM", "serving"};
	if (module == "sglang.launch_server") return {"SGLang", "serving"};
	if (module == "torch.distributed.run") return {"PyTorch", "distributed"};
	return {};
}

inline auto label(std::string_view command) -> std::string {
	const auto info = detect(command);
	if (info.runtime.empty()) return {};
	return "[" + std::string(info.runtime) + " " + std::string(info.role) + "] ";
}
} // namespace Workload
