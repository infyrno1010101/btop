// SPDX-License-Identifier: Apache-2.0
#include "btop_workload.hpp"
#include "btop_shared.hpp"
#include "btop_config.hpp"
#include <algorithm>
#include <gtest/gtest.h>

TEST(workloads, runtime_signatures) {
	EXPECT_EQ(Workload::detect("/opt/bin/llama-server -m model.gguf").runtime, "llama.cpp");
	EXPECT_EQ(Workload::detect("llama-cli -m model.gguf").role, "inference");
	EXPECT_EQ(Workload::detect("ollama serve").role, "serving");
	EXPECT_EQ(Workload::detect("ollama runner --model x").role, "worker");
	EXPECT_EQ(Workload::detect("vllm serve model").runtime, "vLLM");
	EXPECT_EQ(Workload::detect("/venv/bin/python3.12 -m sglang.launch_server").runtime, "SGLang");
	EXPECT_EQ(Workload::detect("python -m vllm.entrypoints.openai.api_server").runtime, "vLLM");
	EXPECT_EQ(Workload::detect("torchrun train.py").role, "distributed");
	EXPECT_EQ(Workload::detect("python3 -m torch.distributed.run infer.py").role, "distributed");
}

TEST(workloads, do_not_guess_from_arbitrary_arguments) {
	for (const auto command : {"", "  ", "python train.py", "python3 script.py --model vllm",
		"echo llama-server", "cat /logs/llama-server", "ollama list", "vllm --help",
		"my-llama-server", "python3.fake -m sglang.launch_server", "sh -c llama-server"})
		EXPECT_TRUE(Workload::detect(command).runtime.empty()) << command;
}

TEST(workloads, labels_and_filtering) {
	EXPECT_EQ(Workload::label("llama-server"), "[llama.cpp serving] ");
	EXPECT_TRUE(Workload::label("python script.py").empty());
	Proc::proc_info process;
	process.cmd = "llama-server -m /models/example.gguf";
	EXPECT_TRUE(Proc::matches_filter(process, "ai:"));
	EXPECT_TRUE(Proc::matches_filter(process, "ai:LLAMA"));
	EXPECT_TRUE(Proc::matches_filter(process, "ai:serving"));
	EXPECT_TRUE(Proc::matches_filter(process, "ai:example"));
	EXPECT_FALSE(Proc::matches_filter(process, "ai:vllm"));
	EXPECT_TRUE(Proc::matches_filter(process, "example"));
	process.cmd = "cat example.gguf";
	EXPECT_FALSE(Proc::matches_filter(process, "ai:"));
}

namespace {
Proc::proc_info process(size_t pid, size_t ppid, const std::string& command, const std::string& user = "alice") {
	Proc::proc_info p;
	p.pid = pid; p.ppid = ppid; p.cmd = command; p.user = user;
	p.cpu_p = 1; p.cpu_c = 2; p.mem = 100; p.threads = 1;
#if defined(GPU_SUPPORT) && defined(__linux__)
	p.gpu_p = 5; p.gpu_mem = 200;
#endif
	return p;
}
}

TEST(workloads, groups_descendants_but_keeps_jobs_separate) {
	std::vector<Proc::proc_info> procs = {
		process(10, 1, "torchrun train.py"), process(11, 10, "python train.py"),
		process(12, 11, "python worker.py"), process(20, 1, "llama-server"),
		process(21, 20, "helper"), process(30, 1, "python unrelated.py"),
		process(31, 10, "helper", "bob")};
	Proc::assign_workloads(procs);
	EXPECT_EQ(procs[0].workload_members, 3);
	EXPECT_EQ(procs[1].workload_root, 10);
	EXPECT_EQ(procs[2].workload_root, 10);
	EXPECT_EQ(procs[2].workload_role, "worker");
	EXPECT_EQ(procs[3].workload_members, 2);
	EXPECT_EQ(procs[4].workload_root, 20);
	EXPECT_EQ(procs[5].workload_root, 0);
	EXPECT_EQ(procs[6].workload_root, 0);
	EXPECT_TRUE(Proc::matches_filter(procs[2], "ai:PyTorch"));
	EXPECT_TRUE(Proc::matches_filter(procs[2], "ai:worker"));
	EXPECT_EQ(Proc::workload_label(procs[0]), "[PyTorch distributed; 3 processes] ");
	EXPECT_EQ(Proc::workload_label(procs[2]), "[PyTorch worker; job 10] ");
}

TEST(workloads, nested_runtimes_are_one_job_and_refresh_membership) {
	std::vector<Proc::proc_info> procs = {
		process(10, 1, "ollama serve"), process(11, 10, "ollama runner"), process(12, 11, "helper")};
	Proc::assign_workloads(procs);
	EXPECT_EQ(procs[0].workload_members, 3);
	EXPECT_EQ(procs[1].workload_root, 10);
	// The launcher exits: the surviving recognized runner becomes the new root.
	procs.erase(procs.begin());
	Proc::assign_workloads(procs);
	EXPECT_EQ(procs[0].workload_root, 11);
	EXPECT_EQ(procs[0].workload_members, 2);
	EXPECT_EQ(procs[1].workload_root, 11);
	// Reuse of a PID with an unrelated command must clear old membership.
	procs[0].cmd = "unrelated";
	Proc::assign_workloads(procs);
	EXPECT_EQ(procs[0].workload_root, 0);
	EXPECT_EQ(procs[1].workload_root, 0);
	EXPECT_EQ(procs[0].workload_members, 0);
}

TEST(workloads, malformed_ancestry_and_dead_processes) {
	std::vector<Proc::proc_info> procs = {
		process(10, 11, "llama-server"), process(11, 10, "helper"),
		process(20, 999, "llama-server"), process(21, 20, "helper")};
	procs[3].state = 'X';
	Proc::assign_workloads(procs);
	EXPECT_EQ(procs[0].workload_root, 0);
	EXPECT_EQ(procs[1].workload_root, 0);
	EXPECT_EQ(procs[2].workload_members, 1);
	EXPECT_EQ(procs[3].workload_root, 0);
}

TEST(workloads, tree_aggregates_each_descendant_once) {
	const auto saved_aggregate = Config::getB("proc_aggregate");
	Config::set("proc_aggregate", true);
	std::vector<Proc::proc_info> procs = {
		process(10, 1, "torchrun train.py"), process(11, 10, "python train.py"),
		process(12, 11, "helper")};
	Proc::assign_workloads(procs);
	std::ranges::sort(procs, {}, &Proc::proc_info::ppid);
	std::vector<Proc::tree_proc> trees;
	procs[0].filtered = true; // A previously hidden root must reappear when it matches.
	Proc::_tree_gen(procs[0], procs, trees, 0, false, "ai:", false, false, true);
	EXPECT_FALSE(procs[0].filtered);
	EXPECT_EQ(procs[0].cpu_p, 3);
	EXPECT_EQ(procs[0].mem, 300);
	EXPECT_EQ(procs[0].threads, 3);
#if defined(GPU_SUPPORT) && defined(__linux__)
	EXPECT_EQ(procs[0].gpu_p, 15);
	EXPECT_EQ(procs[0].gpu_mem, 600);
#endif
	EXPECT_EQ(trees.size(), 1);
	EXPECT_EQ(trees[0].children.size(), 1);
	Config::set("proc_aggregate", saved_aggregate);
}

TEST(workloads, view_restores_original_preferences) {
	const bool tree = Config::getB("proc_tree"), aggregate = Config::getB("proc_aggregate");
	const auto filter = Config::getS("proc_filter");
	Config::set("proc_tree", false);
	Config::set("proc_aggregate", false);
	Config::set("proc_filter", std::string("original"));
	Proc::toggle_workload_view();
	EXPECT_TRUE(Config::getB("proc_workloads"));
	EXPECT_TRUE(Config::getB("proc_tree"));
	EXPECT_TRUE(Config::getB("proc_aggregate"));
	EXPECT_EQ(Config::getS("proc_filter"), "ai:");
	Proc::toggle_workload_view();
	EXPECT_FALSE(Config::getB("proc_workloads"));
	EXPECT_FALSE(Config::getB("proc_tree"));
	EXPECT_FALSE(Config::getB("proc_aggregate"));
	EXPECT_EQ(Config::getS("proc_filter"), "original");
	Config::set("proc_tree", tree);
	Config::set("proc_aggregate", aggregate);
	Config::set("proc_filter", filter);
}

TEST(workloads, workload_totals_respect_user_boundaries) {
	const bool aggregate = Config::getB("proc_aggregate");
	Config::set("proc_aggregate", true);
	Config::set("proc_workloads", true);
	std::vector<Proc::proc_info> procs = {
		process(10, 1, "llama-server"), process(11, 10, "helper"),
		process(12, 10, "helper", "bob")};
	Proc::assign_workloads(procs);
	std::ranges::sort(procs, {}, &Proc::proc_info::ppid);
	std::vector<Proc::tree_proc> trees;
	Proc::_tree_gen(procs[0], procs, trees, 0, false, "ai:", false, false, true);
	EXPECT_EQ(procs[0].workload_members, 2);
	EXPECT_EQ(procs[0].mem, 200);
	EXPECT_TRUE(procs[2].filtered);
#if defined(GPU_SUPPORT) && defined(__linux__)
	EXPECT_EQ(procs[0].gpu_mem, 400);
#endif
	Config::set("proc_workloads", false);
	Config::set("proc_aggregate", aggregate);
}
