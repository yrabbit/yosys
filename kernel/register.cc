/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/satgen.h"
#include "kernel/json.h"
#include "kernel/gzip.h"
#include "kernel/log_help.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

YOSYS_NAMESPACE_BEGIN

#define MAX_REG_COUNT 1000

bool echo_mode = false;
Pass *first_queued_pass;
Pass *current_pass;

std::map<std::string, Frontend*> frontend_register;
std::map<std::string, Pass*> pass_register;
std::map<std::string, Backend*> backend_register;

std::vector<std::string> Frontend::next_args;

Pass::Pass(std::string name, std::string short_help, source_location location) : 
	pass_name(name), short_help(short_help), location(location)
{
	next_queued_pass = first_queued_pass;
	first_queued_pass = this;
	call_counter = 0;
	runtime_ns = 0;
}

void Pass::run_register()
{
	if (pass_register.count(pass_name) && !replace_existing_pass())
		log_error("Unable to register pass '%s', pass already exists!\n", pass_name.c_str());
	pass_register[pass_name] = this;
}

void Pass::init_register()
{
	vector<Pass*> added_passes;
	while (first_queued_pass) {
		added_passes.push_back(first_queued_pass);
		first_queued_pass->run_register();
		first_queued_pass = first_queued_pass->next_queued_pass;
	}
	for (auto added_pass : added_passes)
		added_pass->on_register();
}

void Pass::done_register()
{
	for (auto &it : pass_register)
		it.second->on_shutdown();

	frontend_register.clear();
	pass_register.clear();
	backend_register.clear();
	log_assert(first_queued_pass == NULL);
}

void Pass::on_register()
{
}

void Pass::on_shutdown()
{
}

Pass::~Pass()
{
}

Pass::pre_post_exec_state_t Pass::pre_execute()
{
	pre_post_exec_state_t state;
	call_counter++;
	state.begin_ns = PerformanceTimer::query();
	state.parent_pass = current_pass;
	current_pass = this;
	clear_flags();
	return state;
}

void Pass::post_execute(Pass::pre_post_exec_state_t state)
{
	IdString::checkpoint();
	log_suppressed();

	int64_t time_ns = PerformanceTimer::query() - state.begin_ns;
	runtime_ns += time_ns;
	current_pass = state.parent_pass;
	if (current_pass)
		current_pass->runtime_ns -= time_ns;
}

void Pass::help()
{
	auto prettyHelp = PrettyHelp();
	if (formatted_help()) {
		prettyHelp.log_help();
	} else {
		log("\n");
		log("No help message for command `%s'.\n", pass_name.c_str());
		log("\n");
	}
}

bool Pass::formatted_help()
{
	return false;
}

void Pass::clear_flags()
{
}

void Pass::cmd_log_args(const std::vector<std::string> &args)
{
	if (args.size() <= 1)
		return;
	log("Full command line:");
	for (size_t i = 0; i < args.size(); i++)
		log(" %s", args[i].c_str());
	log("\n");
}

void Pass::cmd_error(const std::vector<std::string> &args, size_t argidx, std::string msg)
{
	std::string command_text;
	int error_pos = 0;

	for (size_t i = 0; i < args.size(); i++) {
		if (i < argidx)
			error_pos += args[i].size() + 1;
		command_text = command_text + (command_text.empty() ? "" : " ") + args[i];
	}

	log("\nSyntax error in command `%s':\n", command_text.c_str());
	help();

	log_cmd_error("Command syntax error: %s\n> %s\n> %*s^\n",
			msg.c_str(), command_text.c_str(), error_pos, "");
}

void Pass::extra_args(std::vector<std::string> args, size_t argidx, RTLIL::Design *design, bool select)
{
	for (; argidx < args.size(); argidx++)
	{
		std::string arg = args[argidx];

		if (arg.compare(0, 1, "-") == 0)
			cmd_error(args, argidx, "Unknown option or option in arguments.");

		if (!select)
			cmd_error(args, argidx, "Extra argument.");

		handle_extra_select_args(this, args, argidx, args.size(), design);
		break;
	}
	// cmd_log_args(args);
}

void Pass::call(RTLIL::Design *design, std::string command)
{
	std::vector<std::string> args;

	std::string cmd_buf = command;
	std::string tok = next_token(cmd_buf, " \t\r\n", true);

	if (tok.empty())
		return;

	if (tok[0] == '!') {
#if !defined(YOSYS_DISABLE_SPAWN)
		cmd_buf = command.substr(command.find('!') + 1);
		while (!cmd_buf.empty() && (cmd_buf.back() == ' ' || cmd_buf.back() == '\t' ||
				cmd_buf.back() == '\r' || cmd_buf.back() == '\n'))
			cmd_buf.resize(cmd_buf.size()-1);
		log_header(design, "Shell command: %s\n", cmd_buf.c_str());
		int retCode = run_command(cmd_buf);
		if (retCode != 0)
			log_cmd_error("Shell command returned error code %d.\n", retCode);
		return;
#else
		log_cmd_error("Shell is not available.\n");
#endif
	}

	while (!tok.empty()) {
		if (tok[0] == '#') {
			int stop;
			for (stop = 0; stop < GetSize(cmd_buf); stop++)
				if (cmd_buf[stop] == '\r' || cmd_buf[stop] == '\n')
					break;
			cmd_buf = cmd_buf.substr(stop);
		} else
		if (tok.back() == ';') {
			int num_semikolon = 0;
			while (!tok.empty() && tok.back() == ';')
				tok.resize(tok.size()-1), num_semikolon++;
			if (!tok.empty())
				args.push_back(tok);
			call(design, args);
			args.clear();
			if (num_semikolon == 2)
				call(design, "clean");
			if (num_semikolon == 3)
				call(design, "clean -purge");
		} else
			args.push_back(tok);
		bool found_nl = false;
		for (auto c : cmd_buf) {
			if (c == ' ' || c == '\t')
				continue;
			if (c == '\r' || c == '\n')
				found_nl = true;
			break;
		}
		if (found_nl) {
			call(design, args);
			args.clear();
		}
		tok = next_token(cmd_buf, " \t\r\n", true);
	}

	call(design, args);
}

void Pass::call(RTLIL::Design *design, std::vector<std::string> args)
{
	if (args.size() == 0 || args[0][0] == '#' || args[0][0] == ':')
		return;

	if (echo_mode) {
		log("%s", create_prompt(design, 0));
		for (size_t i = 0; i < args.size(); i++)
			log("%s%s", i ? " " : "", args[i].c_str());
		log("\n");
	}

	if (pass_register.count(args[0]) == 0)
		log_cmd_error("No such command: %s (type 'help' for a command overview)\n", args[0].c_str());

	if (pass_register[args[0]]->experimental_flag)
		log_experimental("%s", args[0].c_str());

	size_t orig_sel_stack_pos = design->selection_stack.size();
	auto state = pass_register[args[0]]->pre_execute();
	pass_register[args[0]]->execute(args, design);
	pass_register[args[0]]->post_execute(state);
	while (design->selection_stack.size() > orig_sel_stack_pos)
		design->pop_selection();
}

void Pass::call_on_selection(RTLIL::Design *design, const RTLIL::Selection &selection, std::string command)
{
	std::string backup_selected_active_module = design->selected_active_module;
	design->selected_active_module.clear();
	design->push_selection(selection);

	Pass::call(design, command);

	design->pop_selection();
	design->selected_active_module = backup_selected_active_module;
}

void Pass::call_on_selection(RTLIL::Design *design, const RTLIL::Selection &selection, std::vector<std::string> args)
{
	std::string backup_selected_active_module = design->selected_active_module;
	design->selected_active_module.clear();
	design->push_selection(selection);

	Pass::call(design, args);

	design->pop_selection();
	design->selected_active_module = backup_selected_active_module;
}

void Pass::call_on_module(RTLIL::Design *design, RTLIL::Module *module, std::string command)
{
	std::string backup_selected_active_module = design->selected_active_module;
	design->selected_active_module = module->name.str();
	design->push_empty_selection();
	design->select(module);

	Pass::call(design, command);

	design->pop_selection();
	design->selected_active_module = backup_selected_active_module;
}

void Pass::call_on_module(RTLIL::Design *design, RTLIL::Module *module, std::vector<std::string> args)
{
	std::string backup_selected_active_module = design->selected_active_module;
	design->selected_active_module = module->name.str();
	design->push_empty_selection();
	design->select(module);

	Pass::call(design, args);

	design->pop_selection();
	design->selected_active_module = backup_selected_active_module;
}

bool ScriptPass::check_label(std::string label, std::string info)
{
	if (active_design == nullptr) {
		log("\n");
		if (info.empty())
			log("    %s:\n", label.c_str());
		else
			log("    %s:    %s\n", label.c_str(), info.c_str());
		return true;
	} else {
		if (!active_run_from.empty() && active_run_from == active_run_to) {
			block_active = (label == active_run_from);
		} else {
			if (label == active_run_from)
				block_active = true;
			if (label == active_run_to)
				block_active = false;
		}
		return block_active;
	}
}

void ScriptPass::run(std::string command, std::string info)
{
	if (active_design == nullptr) {
		if (info.empty())
			log("        %s\n", command.c_str());
		else
			log("        %s    %s\n", command.c_str(), info.c_str());
	} else {
		Pass::call(active_design, command);
		active_design->check();
	}
}

void ScriptPass::run_nocheck(std::string command, std::string info)
{
	if (active_design == nullptr) {
		if (info.empty())
			log("        %s\n", command.c_str());
		else
			log("        %s    %s\n", command.c_str(), info.c_str());
	} else {
		Pass::call(active_design, command);
	}
}

void ScriptPass::run_script(RTLIL::Design *design, std::string run_from, std::string run_to)
{
	help_mode = false;
	active_design = design;
	block_active = run_from.empty();
	active_run_from = run_from;
	active_run_to = run_to;
	script();
}

void ScriptPass::help_script()
{
	clear_flags();
	help_mode = true;
	active_design = nullptr;
	block_active = true;
	active_run_from.clear();
	active_run_to.clear();
	script();
}

Frontend::Frontend(std::string name, std::string short_help, source_location location) :
		Pass(name.rfind("=", 0) == 0 ? name.substr(1) : "read_" + name, short_help, location),
		frontend_name(name.rfind("=", 0) == 0 ? name.substr(1) : name)
{
}

void Frontend::run_register()
{
	if (pass_register.count(pass_name) && !replace_existing_pass())
		log_error("Unable to register pass '%s', pass already exists!\n", pass_name.c_str());
	pass_register[pass_name] = this;

	if (frontend_register.count(frontend_name) && !replace_existing_pass())
		log_error("Unable to register frontend '%s', frontend already exists!\n", frontend_name.c_str());
	frontend_register[frontend_name] = this;
}

Frontend::~Frontend()
{
}

void Frontend::execute(std::vector<std::string> args, RTLIL::Design *design)
{
	log_assert(next_args.empty());
	do {
		std::istream *f = NULL;
		next_args.clear();
		auto state = pre_execute();
		execute(f, std::string(), args, design);
		post_execute(state);
		args = next_args;
		delete f;
	} while (!args.empty());
}

FILE *Frontend::current_script_file = NULL;
std::string Frontend::last_here_document;

void Frontend::extra_args(std::istream *&f, std::string &filename, std::vector<std::string> args, size_t argidx, bool bin_input)
{
	bool called_with_fp = f != NULL;

	next_args.clear();

	if (argidx < args.size())
	{
		std::string arg = args[argidx];

		if (arg.compare(0, 1, "-") == 0)
			cmd_error(args, argidx, "Unknown option or option in arguments.");
		if (f != NULL)
			cmd_error(args, argidx, "Extra filename argument in direct file mode.");

		filename = arg;
		//Accommodate heredocs with EOT marker spaced out from "<<", e.g. "<< EOT" vs. "<<EOT"
		if (filename == "<<" && argidx+1 < args.size())
			filename += args[++argidx];
		if (filename.compare(0, 2, "<<") == 0) {
			if (filename.size() <= 2)
				log_error("Missing EOT marker in here document!\n");
			std::string eot_marker = filename.substr(2);
			if (Frontend::current_script_file == nullptr)
				filename = "<stdin>";
			last_here_document.clear();
			while (1) {
				std::string buffer;
				char block[4096];
				while (1) {
					if (fgets(block, 4096, Frontend::current_script_file == nullptr? stdin : Frontend::current_script_file) == nullptr)
						log_error("Unexpected end of file in here document '%s'!\n", filename.c_str());
					buffer += block;
					if (buffer.size() > 0 && (buffer[buffer.size() - 1] == '\n' || buffer[buffer.size() - 1] == '\r'))
						break;
				}
				size_t indent = buffer.find_first_not_of(" \t\r\n");
				if (indent != std::string::npos && buffer.compare(indent, eot_marker.size(), eot_marker) == 0)
					break;
				last_here_document += buffer;
			}
			f = new std::istringstream(last_here_document);
		} else {
			rewrite_filename(filename);
			vector<string> filenames = glob_filename(filename);
			filename = filenames.front();
			if (GetSize(filenames) > 1) {
				next_args.insert(next_args.end(), args.begin(), args.begin()+argidx);
				next_args.insert(next_args.end(), filenames.begin()+1, filenames.end());
			}
			yosys_input_files.insert(filename);
			f = uncompressed(filename, bin_input ? std::ifstream::binary : std::ifstream::in);
		}

		for (size_t i = argidx+1; i < args.size(); i++)
			if (args[i].compare(0, 1, "-") == 0)
				cmd_error(args, i, "Found option, expected arguments.");

		if (argidx+1 < args.size()) {
			if (next_args.empty())
				next_args.insert(next_args.end(), args.begin(), args.begin()+argidx);
			next_args.insert(next_args.end(), args.begin()+argidx+1, args.end());
			args.erase(args.begin()+argidx+1, args.end());
		}
	}

	if (f == NULL)
		cmd_error(args, argidx, "No filename given.");

	if (called_with_fp)
		args.push_back(filename);
	args[0] = pass_name;
	// cmd_log_args(args);
}

void Frontend::frontend_call(RTLIL::Design *design, std::istream *f, std::string filename, std::string command)
{
	std::vector<std::string> args;
	char *s = strdup(command.c_str());
	for (char *p = strtok(s, " \t\r\n"); p; p = strtok(NULL, " \t\r\n"))
		args.push_back(p);
	free(s);
	frontend_call(design, f, filename, args);
}

void Frontend::frontend_call(RTLIL::Design *design, std::istream *f, std::string filename, std::vector<std::string> args)
{
	if (args.size() == 0)
		return;
	if (frontend_register.count(args[0]) == 0)
		log_cmd_error("No such frontend: %s\n", args[0].c_str());

	if (f != NULL) {
		auto state = frontend_register[args[0]]->pre_execute();
		frontend_register[args[0]]->execute(f, filename, args, design);
		frontend_register[args[0]]->post_execute(state);
	} else if (filename == "-") {
		std::istream *f_cin = &std::cin;
		auto state = frontend_register[args[0]]->pre_execute();
		frontend_register[args[0]]->execute(f_cin, "<stdin>", args, design);
		frontend_register[args[0]]->post_execute(state);
	} else {
		if (!filename.empty())
			args.push_back(filename);
		frontend_register[args[0]]->execute(args, design);
	}
}

Backend::Backend(std::string name, std::string short_help, source_location location) :
		Pass(name.rfind("=", 0) == 0 ? name.substr(1) : "write_" + name, short_help, location),
		backend_name(name.rfind("=", 0) == 0 ? name.substr(1) : name)
{
}

void Backend::run_register()
{
	if (pass_register.count(pass_name))
		log_error("Unable to register pass '%s', pass already exists!\n", pass_name.c_str());
	pass_register[pass_name] = this;

	if (backend_register.count(backend_name))
		log_error("Unable to register backend '%s', backend already exists!\n", backend_name.c_str());
	backend_register[backend_name] = this;
}

Backend::~Backend()
{
}

void Backend::execute(std::vector<std::string> args, RTLIL::Design *design)
{
	std::ostream *f = NULL;
	auto state = pre_execute();
	execute(f, std::string(), args, design);
	post_execute(state);
	if (f != &std::cout)
		delete f;
}

void Backend::extra_args(std::ostream *&f, std::string &filename, std::vector<std::string> args, size_t argidx, bool bin_output)
{
	bool called_with_fp = f != NULL;

	for (; argidx < args.size(); argidx++)
	{
		std::string arg = args[argidx];

		if (arg.compare(0, 1, "-") == 0 && arg != "-")
			cmd_error(args, argidx, "Unknown option or option in arguments.");
		if (f != NULL)
			cmd_error(args, argidx, "Extra filename argument in direct file mode.");

		if (arg == "-") {
			filename = "<stdout>";
			f = &std::cout;
			continue;
		}

		filename = arg;
		rewrite_filename(filename);
		if (filename.size() > 3 && filename.compare(filename.size()-3, std::string::npos, ".gz") == 0) {
#ifdef YOSYS_ENABLE_ZLIB
			gzip_ostream *gf = new gzip_ostream;
			if (!gf->open(filename)) {
				delete gf;
				log_cmd_error("Can't open output file `%s' for writing: %s\n", filename.c_str(), strerror(errno));
			}
			yosys_output_files.insert(filename);
			f = gf;
#else
			log_cmd_error("Yosys is compiled without zlib support, unable to write gzip output.\n");
#endif
		} else {
			std::ofstream *ff = new std::ofstream;
			ff->open(filename.c_str(), bin_output ? (std::ofstream::trunc | std::ofstream::binary) : std::ofstream::trunc);
			yosys_output_files.insert(filename);
			if (ff->fail()) {
				delete ff;
				log_cmd_error("Can't open output file `%s' for writing: %s\n", filename.c_str(), strerror(errno));
			}
			f = ff;
		}
	}

	if (called_with_fp)
		args.push_back(filename);
	args[0] = pass_name;
	// cmd_log_args(args);

	if (f == NULL) {
		filename = "<stdout>";
		f = &std::cout;
	}
}

void Backend::backend_call(RTLIL::Design *design, std::ostream *f, std::string filename, std::string command)
{
	std::vector<std::string> args;
	char *s = strdup(command.c_str());
	for (char *p = strtok(s, " \t\r\n"); p; p = strtok(NULL, " \t\r\n"))
		args.push_back(p);
	free(s);
	backend_call(design, f, filename, args);
}

void Backend::backend_call(RTLIL::Design *design, std::ostream *f, std::string filename, std::vector<std::string> args)
{
	if (args.size() == 0)
		return;
	if (backend_register.count(args[0]) == 0)
		log_cmd_error("No such backend: %s\n", args[0].c_str());

	size_t orig_sel_stack_pos = design->selection_stack.size();

	if (f != NULL) {
		auto state = backend_register[args[0]]->pre_execute();
		backend_register[args[0]]->execute(f, filename, args, design);
		backend_register[args[0]]->post_execute(state);
	} else if (filename == "-") {
		std::ostream *f_cout = &std::cout;
		auto state = backend_register[args[0]]->pre_execute();
		backend_register[args[0]]->execute(f_cout, "<stdout>", args, design);
		backend_register[args[0]]->post_execute(state);
	} else {
		if (!filename.empty())
			args.push_back(filename);
		backend_register[args[0]]->execute(args, design);
	}

	while (design->selection_stack.size() > orig_sel_stack_pos)
		design->pop_selection();
}

struct SimHelper {
	string name;
	inline string filesafe_name() {
		if (name.at(0) == '$')
			if (name.at(1) == '_')
				return "gate" + name.substr(1);
			else
				return "word_" + name.substr(1);
		else
			return name;
	}
	string title;
	string ports;
	string source;
	string desc;
	string code;
	string group;
	string ver;
	string tags;
};

static bool is_code_getter(string name) {
	return *(--(name.end())) == '+';
}

static string get_cell_name(string name) {
	return is_code_getter(name) ? name.substr(0, name.length()-1) : name;
}

static void log_warning_flags(Pass *pass) {
	bool has_warnings = false;
	const string name = pass->pass_name;
	if (pass->experimental_flag) {
		if (!has_warnings) log("\n");
		has_warnings = true;
		log("WARNING: THE '%s' COMMAND IS EXPERIMENTAL.\n", name.c_str());
	}
	if (pass->internal_flag) {
		if (!has_warnings) log("\n");
		has_warnings = true;
		log("WARNING: THE '%s' COMMAND IS INTENDED FOR INTERNAL DEVELOPER USE ONLY.\n", name.c_str());
	}
	if (has_warnings)
		log("\n");
}

static struct CellHelpMessages {
	dict<string, SimHelper> cell_help;
	CellHelpMessages() {
#include "techlibs/common/simlib_help.inc"
#include "techlibs/common/simcells_help.inc"
		cell_help.sort();
	}
	bool contains(string name) { return cell_help.count(get_cell_name(name)) > 0; }
	SimHelper get(string name) { return cell_help[get_cell_name(name)]; }
} cell_help_messages;

struct HelpPass : public Pass {
	HelpPass() : Pass("help", "display help messages") { }
	void help() override
	{
		log("\n");
		log("    help  ................  list all commands\n");
		log("    help <command>  ......  print help message for given command\n");
		log("    help -all  ...........  print complete command reference\n");
		log("\n");
		log("    help -cells ..........  list all cell types\n");
		log("    help <celltype>  .....  print help message for given cell type\n");
		log("    help <celltype>+  ....  print verilog code for given cell type\n");
		log("\n");
	}
	bool dump_cmds_json(PrettyJson &json) {
		// init json
		json.begin_object();
		json.entry("version", "Yosys command reference");
		json.entry("generator", yosys_version_str);

		bool raise_error = false;
		std::map<string, vector<string>> groups;

		json.name("cmds"); json.begin_object();
		// iterate over commands
		for (auto &it : pass_register) {
			auto name = it.first;
			auto pass = it.second;
			auto title = pass->short_help;

			auto cmd_help = PrettyHelp();
			auto has_pretty_help = pass->formatted_help();

			if (!has_pretty_help) {
				enum PassUsageState {
					PUState_none,
					PUState_signature,
					PUState_options,
					PUState_optionbody,
				};

				source_location null_source;
				string current_buffer = "";
				auto root_listing = cmd_help.get_root();
				auto current_listing = root_listing;

				// dump command help
				std::ostringstream buf;
				log_streams.push_back(&buf);
				pass->help();
				log_streams.pop_back();
				std::stringstream ss;
				ss << buf.str();

				// parse command help
				size_t def_strip_count = 0;
				auto current_state = PUState_none;
				auto catch_verific = false;
				auto blank_lines = 2;
				for (string line; std::getline(ss, line, '\n');) {
					// find position of first non space character
					std::size_t first_pos = line.find_first_not_of(" \t");
					std::size_t last_pos = line.find_last_not_of(" \t");
					if (first_pos == std::string::npos) {
						switch (current_state)
						{
						case PUState_signature:
							root_listing->usage(current_buffer, null_source);
							current_listing = root_listing;
							current_state = PUState_none;
							current_buffer = "";
							break;
						case PUState_none:
						case PUState_optionbody:
							blank_lines += 1;
							break;
						default:
							break;
						}
						// skip empty lines
						continue;
					}

					// strip leading and trailing whitespace
					std::string stripped_line = line.substr(first_pos, last_pos - first_pos +1);
					bool IsDefinition = stripped_line[0] == '-';
					IsDefinition &= stripped_line[1] != ' ' && stripped_line[1] != '>';
					bool IsDedent = def_strip_count && first_pos < def_strip_count;
					bool IsIndent = def_strip_count < first_pos;

					// line looks like a signature
					bool IsSignature = stripped_line.find(name) == 0 && (stripped_line.length() == name.length() || stripped_line.at(name.size()) == ' ');

					if (IsSignature && first_pos <= 4 && (blank_lines >= 2 || current_state == PUState_signature)) {
						if (current_state == PUState_options || current_state == PUState_optionbody) {
							current_listing->codeblock(current_buffer, "none", null_source);
							current_buffer = "";
						} else if (current_state == PUState_signature) {
							root_listing->usage(current_buffer, null_source);
							current_buffer = "";
						} else if (current_state == PUState_none && !current_buffer.empty()) {
							current_listing->codeblock(current_buffer, "none", null_source);
							current_buffer = "";
						}
						current_listing = root_listing;
						current_state = PUState_signature;
						def_strip_count = first_pos;
						catch_verific = false;
					} else if (IsDedent) {
						def_strip_count = first_pos;
						if (current_state == PUState_optionbody) {
							if (!current_buffer.empty()) {
								current_listing->codeblock(current_buffer, "none", null_source);
								current_buffer = "";
							}
							if (IsIndent) {
								current_state = PUState_options;
								current_listing = root_listing->back();
							} else {
								current_state = PUState_none;
								current_listing = root_listing;
							}
						} else {
							current_state = PUState_none;
						}
					}

					if (IsDefinition && !catch_verific && current_state != PUState_signature) {
						if (!current_buffer.empty()) {
							current_listing->codeblock(current_buffer, "none", null_source);
							current_buffer = "";
						}
						current_state = PUState_options;
						current_listing = root_listing->open_option(stripped_line, null_source);
						def_strip_count = first_pos;
					} else {
						if (current_state == PUState_options) {
							current_state = PUState_optionbody;
						}
						if (current_buffer.empty())
							current_buffer = stripped_line;
						else if (current_state == PUState_signature && IsIndent)
							current_buffer += stripped_line;
						else if (current_state == PUState_none) {
							current_buffer += (blank_lines > 0 ? "\n\n" : "\n") + line;
						} else
							current_buffer += (blank_lines > 0 ? "\n\n" : "\n") + stripped_line;
						if (stripped_line.compare("Command file parser supports following commands in file:") == 0)
							catch_verific = true;
					}
					blank_lines = 0;
				}

				if (!current_buffer.empty()) {
					if (current_buffer.size() > 64 && current_buffer.substr(0, 64).compare("The following commands are executed by this synthesis command:\n\n") == 0) {
						current_listing->paragraph(current_buffer.substr(0, 62), null_source);
						current_listing->codeblock(current_buffer.substr(64), "yoscrypt", null_source);
					} else
						current_listing->codeblock(current_buffer, "none", null_source);
					current_buffer = "";
				}
			}

			// attempt auto group
			if (!cmd_help.has_group()) {
				string source_file = pass->location.file_name();
				bool has_source = source_file.compare("unknown") != 0;
				if (pass->internal_flag)
					cmd_help.group = "internal";
				else if (source_file.find("backends/") == 0 || (!has_source && name.find("read_") == 0))
					cmd_help.group = "backends";
				else if (source_file.find("frontends/") == 0 || (!has_source && name.find("write_") == 0))
					cmd_help.group = "frontends";
				else if (has_source) {
					auto last_slash = source_file.find_last_of('/');
					if (last_slash != string::npos) {
						auto parent_path = source_file.substr(0, last_slash);
						cmd_help.group = parent_path;
					}
				}
				// implicit !has_source
				else if (name.find("equiv") == 0)
					cmd_help.group = "passes/equiv";
				else if (name.find("fsm") == 0)
					cmd_help.group = "passes/fsm";
				else if (name.find("memory") == 0)
					cmd_help.group = "passes/memory";
				else if (name.find("opt") == 0)
					cmd_help.group = "passes/opt";
				else if (name.find("proc") == 0)
					cmd_help.group = "passes/proc";
			}

			if (groups.count(cmd_help.group) == 0) {
				groups[cmd_help.group] = vector<string>();
			}
			groups[cmd_help.group].push_back(name);

			// write to json
			json.name(name.c_str()); json.begin_object();
			json.entry("title", title);
			json.name("content"); json.begin_array();
			for (auto content : cmd_help.get_content())
				json.value(content->to_json());
			json.end_array();
			json.entry("group", cmd_help.group);
			json.entry("source_file", pass->location.file_name());
			json.entry("source_line", pass->location.line());
			json.entry("source_func", pass->location.function_name());
			json.entry("experimental_flag", pass->experimental_flag);
			json.entry("internal_flag", pass->internal_flag);
			json.end_object();
		}
		json.end_object();

		json.entry("groups", groups);

		json.end_object();
		return raise_error;
	}
	bool dump_cells_json(PrettyJson &json) {
		// init json
		json.begin_object();
		json.entry("version", "Yosys internal cells");
		json.entry("generator", yosys_maybe_version());

		dict<string, vector<string>> groups;
		dict<string, pair<SimHelper, CellType>> cells;

		// iterate over cells
		bool raise_error = false;
		for (auto &it : yosys_celltypes.cell_types) {
			auto name = it.first.str();
			if (cell_help_messages.contains(name)) {
				auto cell_help = cell_help_messages.get(name);
				if (groups.count(cell_help.group) != 0) {
					auto group_cells = &groups.at(cell_help.group);
					group_cells->push_back(name);
				} else {
					auto group_cells = new vector<string>(1, name);
					groups.emplace(cell_help.group, *group_cells);
				}
				auto cell_pair = pair<SimHelper, CellType>(cell_help, it.second);
				cells.emplace(name, cell_pair);
			} else {
				log("ERROR: Missing cell help for cell '%s'.\n", name.c_str());
				raise_error |= true;
			}
		}
		for (auto &it : cell_help_messages.cell_help) {
			if (cells.count(it.first) == 0) {
				log_warning("Found cell model '%s' without matching cell type.\n", it.first.c_str());
			}
		}

		// write to json
		json.name("groups"); json.begin_object();
		groups.sort();
		for (auto &it : groups) {
			json.name(it.first.c_str()); json.value(it.second);
		}
		json.end_object();

		json.name("cells"); json.begin_object();
		cells.sort();
		for (auto &it : cells) {
			auto ch = it.second.first;
			auto ct = it.second.second;
			json.name(ch.name.c_str()); json.begin_object();
			json.name("title"); json.value(ch.title);
			json.name("ports"); json.value(ch.ports);
			json.name("source"); json.value(ch.source);
			json.name("desc"); json.value(ch.desc);
			json.name("code"); json.value(ch.code);
			vector<string> inputs, outputs;
			for (auto &input : ct.inputs)
				inputs.push_back(input.str());
			json.name("inputs"); json.value(inputs);
			for (auto &output : ct.outputs)
				outputs.push_back(output.str());
			json.name("outputs"); json.value(outputs);
			vector<string> properties;
			// CellType properties
			if (ct.is_evaluable) properties.push_back("is_evaluable");
			if (ct.is_combinatorial) properties.push_back("is_combinatorial");
			if (ct.is_synthesizable) properties.push_back("is_synthesizable");
			// SimHelper properties
			size_t last = 0; size_t next = 0;
			while ((next = ch.tags.find(", ", last)) != string::npos) {
				properties.push_back(ch.tags.substr(last, next-last));
				last = next + 2;
			}
			auto final_tag = ch.tags.substr(last);
			if (final_tag.size()) properties.push_back(final_tag);
			json.name("properties"); json.value(properties);
			json.end_object();
		}
		json.end_object();

		json.end_object();
		return raise_error;
	}
	void execute(std::vector<std::string> args, RTLIL::Design*) override
	{
		if (args.size() == 1) {
			log("\n");
			for (auto &it : pass_register)
				log("    %-20s %s\n", it.first.c_str(), it.second->short_help.c_str());
			log("\n");
			log("Type 'help <command>' for more information on a command.\n");
			log("Type 'help -cells' for a list of all cell types.\n");
			log("\n");
			return;
		}

		if (args.size() == 2) {
			if (args[1] == "-all") {
				for (auto &it : pass_register) {
					log("\n\n");
					log("%s  --  %s\n", it.first.c_str(), it.second->short_help.c_str());
					for (size_t i = 0; i < it.first.size() + it.second->short_help.size() + 6; i++)
						log("=");
					log("\n");
					it.second->help();
					log_warning_flags(it.second);
				}
			}
			else if (args[1] == "-cells") {
				log("\n");
				for (auto &it : cell_help_messages.cell_help) {
					SimHelper help_cell = it.second;
					log("    %-15s %s\n", help_cell.name.c_str(), help_cell.ports.c_str());
				}
				log("\n");
				log("Type 'help <cell_type>' for more information on a cell type.\n");
				log("\n");
				return;
			}
			else if (pass_register.count(args[1])) {
				pass_register.at(args[1])->help();
				log_warning_flags(pass_register.at(args[1]));
			}
			else if (cell_help_messages.contains(args[1])) {
				auto help_cell = cell_help_messages.get(args[1]);
				if (is_code_getter(args[1])) {
						log("\n");
						log("%s\n", help_cell.code.c_str());
				} else {
					log("\n    %s %s\n\n", help_cell.name.c_str(), help_cell.ports.c_str());
					if (help_cell.ver == "2" || help_cell.ver == "2a") {
						if (help_cell.title != "") log("%s:\n", help_cell.title.c_str());
						std::stringstream ss;
						ss << help_cell.desc;
						for (std::string line; std::getline(ss, line, '\n');) {
							if (line != "::") log("%s\n", line.c_str());
						}
					} else if (help_cell.desc.length()) {
						log("%s\n", help_cell.desc.c_str());
					} else {
						log("No help message for this cell type found.\n");
					}
					log("\nRun 'help %s+' to display the Verilog model for this cell type.\n", args[1].c_str());
					log("\n");
				}
			}
			else
				log("No such command or cell type: %s\n", args[1].c_str());
			return;
		} else if (args.size() == 3) {
			// this option is undocumented as it is for internal use only
			if (args[1] == "-dump-cmds-json") {
				PrettyJson json;
				if (!json.write_to_file(args[2]))
					log_error("Can't open file `%s' for writing: %s\n", args[2].c_str(), strerror(errno));
				if (dump_cmds_json(json)) {
					log_abort();
				}
			}
			// this option is undocumented as it is for internal use only
			else if (args[1] == "-dump-cells-json") {
				PrettyJson json;
				if (!json.write_to_file(args[2]))
					log_error("Can't open file `%s' for writing: %s\n", args[2].c_str(), strerror(errno));
				if (dump_cells_json(json)) {
					log_error("One or more cells defined in celltypes.h are missing help documentation.\n");
				}
			}
			else
				log("Unknown help command: `%s %s'\n", args[1].c_str(), args[2].c_str());
			return;
		}

		help();
	}
} HelpPass;

struct EchoPass : public Pass {
	EchoPass() : Pass("echo", "turning echoing back of commands on and off") { }
	void help() override
	{
		log("\n");
		log("    echo on\n");
		log("\n");
		log("Print all commands to log before executing them.\n");
		log("\n");
		log("\n");
		log("    echo off\n");
		log("\n");
		log("Do not print all commands to log before executing them. (default)\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design*) override
	{
		if (args.size() > 2)
			cmd_error(args, 2, "Unexpected argument.");

		if (args.size() == 2) {
			if (args[1] == "on")
				echo_mode = true;
			else if (args[1] == "off")
				echo_mode = false;
			else
				cmd_error(args, 1, "Unexpected argument.");
		}

		log("echo %s\n", echo_mode ? "on" : "off");
	}
} EchoPass;

SatSolver *yosys_satsolver_list;
SatSolver *yosys_satsolver;

struct MinisatSatSolver : public SatSolver {
	MinisatSatSolver() : SatSolver("minisat") {
		yosys_satsolver = this;
	}
	ezSAT *create() override {
		return new ezMiniSAT();
	}
} MinisatSatSolver;

struct LicensePass : public Pass {
	LicensePass() : Pass("license", "print license terms") { }
	void help() override
	{
		log("\n");
		log("    license\n");
		log("\n");
		log("This command produces the following notice.\n");
		notice();
	}
	void execute(std::vector<std::string>, RTLIL::Design*) override
	{
		notice();
	}
	void notice()
	{
		log("\n");
		log(" /----------------------------------------------------------------------------\\\n");
		log(" |                                                                            |\n");
		log(" |  yosys -- Yosys Open SYnthesis Suite                                       |\n");
		log(" |                                                                            |\n");
		log(" |  Copyright (C) 2012 - 2025  Claire Xenia Wolf <claire@yosyshq.com>         |\n");
		log(" |                                                                            |\n");
		log(" |  Permission to use, copy, modify, and/or distribute this software for any  |\n");
		log(" |  purpose with or without fee is hereby granted, provided that the above    |\n");
		log(" |  copyright notice and this permission notice appear in all copies.         |\n");
		log(" |                                                                            |\n");
		log(" |  THE SOFTWARE IS PROVIDED \"AS IS\" AND THE AUTHOR DISCLAIMS ALL WARRANTIES  |\n");
		log(" |  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF          |\n");
		log(" |  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR   |\n");
		log(" |  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES    |\n");
		log(" |  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN     |\n");
		log(" |  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF   |\n");
		log(" |  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.            |\n");
		log(" |                                                                            |\n");
		log(" \\----------------------------------------------------------------------------/\n");
		log("\n");
	}
} LicensePass;

YOSYS_NAMESPACE_END
