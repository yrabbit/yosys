/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2021  Marcelina Kościelnicka <mwk@0x04.net>
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
#include "kernel/sigtools.h"
#include "kernel/mem.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

enum class RamKind {
	Logic,
	Distributed,
	Block,
	Huge,
};

enum class MemoryInitKind {
	None,
	Zero,
	Any,
};

enum class PortKind {
	Sr,
	Ar,
	Sw,
	Srsw,
	Arsw,
};

enum class ClkPolKind {
	Any,
	Posedge,
	Negedge,
	Named,
};

enum class RdEnKind {
	None,
	Any,
	WriteImplies,
	WriteExcludes,
};

enum class ResetKind {
	Init,
	Async,
	Sync,
};

enum class ResetValKind {
	None,
	Zero,
	Named,
};

enum class SrstKind {
	SrstOverEn,
	EnOverSrst,
	Any,
};

enum class TransTargetKind {
	Self,
	Other,
	Named,
};

enum class TransKind {
	New,
	NewButBe,
	Old,
};

typedef dict<std::string, Const> Options;

struct StringDef {
	std::string val;
	Options opts, portopts;
};

struct ClkPolDef {
	ClkPolKind kind;
	std::string name;
	Options opts, portopts;
};

struct IntDef {
	int val;
	Options opts, portopts;
};

struct VoidDef {
	Options opts, portopts;
};

struct RdEnDef {
	RdEnKind kind;
	Options opts, portopts;
};

struct ResetValDef {
	ResetKind kind;
	ResetValKind val_kind;
	std::string name;
	Options opts, portopts;
};

struct SrstModeDef {
	SrstKind kind;
	Options opts, portopts;
};

struct WrTransDef {
	TransTargetKind target_kind;
	std::string target_name;
	TransKind kind;
	Options opts, portopts;
};

struct PortGroupDef {
	PortKind kind;
	std::vector<std::string> names;
	Options opts;
	std::vector<StringDef> clock;
	std::vector<ClkPolDef> clkpol;
	std::vector<IntDef> width;
	std::vector<VoidDef> mixwidth;
	std::vector<VoidDef> addrce;
	std::vector<RdEnDef> rden;
	std::vector<ResetValDef> rdrstval;
	std::vector<SrstModeDef> rdsrstmode;
	std::vector<IntDef> wrbe;
	std::vector<StringDef> wrprio;
	std::vector<WrTransDef> wrtrans;
	std::vector<IntDef> wrcs;
};

struct MemoryDimsDef {
	int abits;
	int dbits;
	Options opts;
};

struct MemoryInitDef {
	MemoryInitKind kind;
	Options opts;
};

struct RamStringDef {
	std::string val;
	Options opts;
};

struct RamDef {
	IdString id;
	RamKind kind;
	std::vector<PortGroupDef> ports;
	std::vector<MemoryDimsDef> dims;
	std::vector<MemoryInitDef> init;
	std::vector<RamStringDef> style;
};

struct Library {
	std::vector<RamDef> ram_defs;
	const pool<std::string> defines;
	pool<std::string> defines_unused;

	Library(pool<std::string> defines) : defines(defines), defines_unused(defines) {}

	void prepare() {
		for (auto def: defines_unused) {
			log_warning("define %s not used in the library.\n", def.c_str());
		}
	}
};

struct Parser {
	std::string filename;
	std::ifstream infile;
	int line_number = 0;
	Library &lib;
	std::vector<std::string> tokens;
	int token_idx = 0;
	bool eof = false;

	std::vector<std::pair<std::string, Const>> option_stack;
	std::vector<std::pair<std::string, Const>> portoption_stack;
	RamDef ram;
	PortGroupDef port;
	bool active = true;

	Parser(std::string filename, Library &lib) : filename(filename), lib(lib) {
		// Note: this rewrites the filename we're opening, but not
		// the one we're storing — this is actually correct, so that
		// we keep the original filename for diagnostics.
		rewrite_filename(filename);
		infile.open(filename);
		parse();
		infile.close();
	}

	std::string peek_token() {
		if (eof)
			return "";

		if (token_idx < GetSize(tokens))
			return tokens[token_idx];

		tokens.clear();
		token_idx = 0;

		std::string line;
		while (std::getline(infile, line)) {
			line_number++;
			for (string tok = next_token(line); !tok.empty(); tok = next_token(line)) {
				if (tok[0] == '#')
					break;
				if (tok[tok.size()-1] == ';') {
					tokens.push_back(tok.substr(0, tok.size()-1));
					tokens.push_back(";");
				} else {
					tokens.push_back(tok);
				}
			}
			if (!tokens.empty())
				return tokens[token_idx];
		}

		eof = true;
		return "";
	}

	std::string get_token() {
		std::string res = peek_token();
		if (!eof)
			token_idx++;
		return res;
	}

	IdString get_id() {
		std::string token = get_token();
		if (token.empty() || (token[0] != '$' && token[0] != '\\')) {
			log_error("%s:%d: expected id string, got `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
		return IdString(token);
	}

	std::string get_name() {
		std::string res = get_token();
		bool valid = true;
		// Basic sanity check.
		if (res.empty() || (!isalpha(res[0]) && res[0] != '_'))
			valid = false;
		for (char c: res)
			if (!isalnum(c) && c != '_')
				valid = false;
		if (!valid)
			log_error("%s:%d: expected name, got `%s`.\n", filename.c_str(), line_number, res.c_str());
		return res;
	}

	std::string get_string() {
		std::string token = get_token();
		if (token.size() < 2 || token[0] != '"' || token[token.size()-1] != '"') {
			log_error("%s:%d: expected string, got `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
		return token.substr(1, token.size()-2);
	}

	bool peek_string() {
		std::string token = peek_token();
		return !token.empty() && token[0] == '"';
	}

	int get_int() {
		std::string token = get_token();
		char *endptr;
		long res = strtol(token.c_str(), &endptr, 0);
		if (token.empty() || *endptr || res > INT_MAX) {
			log_error("%s:%d: expected int, got `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
		return res;
	}

	bool peek_int() {
		std::string token = peek_token();
		return !token.empty() && isdigit(token[0]);
	}

	void get_semi() {
		std::string token = get_token();
		if (token != ";") {
			log_error("%s:%d: expected `;`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
	}

	Const get_value() {
		std::string token = peek_token();
		if (!token.empty() && token[0] == '"') {
			std::string s = get_string();
			return Const(s);
		} else {
			return Const(get_int());
		}
	}

	bool enter_ifdef(bool polarity) {
		bool res = active;
		std::string name = get_name();
		lib.defines_unused.erase(name);
		if (lib.defines.count(name)) {
			active = polarity;
		} else {
			active = !polarity;
		}
		return res;
	}

	void enter_else(bool save) {
		get_token();
		active = !active && save;
	}

	void enter_option() {
		std::string name = get_string();
		Const val = get_value();
		option_stack.push_back({name, val});
	}

	void exit_option() {
		option_stack.pop_back();
	}

	Options get_options() {
		Options res;
		for (auto it: option_stack)
			res[it.first] = it.second;
		return res;
	}

	void enter_portoption() {
		std::string name = get_string();
		Const val = get_value();
		portoption_stack.push_back({name, val});
	}

	void exit_portoption() {
		portoption_stack.pop_back();
	}

	Options get_portoptions() {
		Options res;
		for (auto it: portoption_stack)
			res[it.first] = it.second;
		return res;
	}

	void parse_port_block() {
		if (peek_token() == "{") {
			get_token();
			while (peek_token() != "}")
				parse_port_item();
			get_token();
		} else {
			parse_port_item();
		}
	}

	void parse_ram_block() {
		if (peek_token() == "{") {
			get_token();
			while (peek_token() != "}")
				parse_ram_item();
			get_token();
		} else {
			parse_ram_item();
		}
	}

	void parse_top_block() {
		if (peek_token() == "{") {
			get_token();
			while (peek_token() != "}")
				parse_top_item();
			get_token();
		} else {
			parse_top_item();
		}
	}

	void parse_port_item() {
		std::string token = get_token();
		if (token == "ifdef") {
			bool save = enter_ifdef(true);
			parse_port_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_port_block();
			}
			active = save;
		} else if (token == "ifndef") {
			bool save = enter_ifdef(false);
			parse_port_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_port_block();
			}
			active = save;
		} else if (token == "option") {
			enter_option();
			parse_port_block();
			exit_option();
		} else if (token == "portoption") {
			enter_portoption();
			parse_port_block();
			exit_portoption();
		} else if (token == "clock") {
			if (port.kind == PortKind::Ar) {
				log_error("%s:%d: `clock` not allowed in async read port.\n", filename.c_str(), line_number);
			}
			StringDef def;
			token = peek_token();
			if (token == "any") {
				get_token();
			} else {
				def.val = get_string();
			}
			get_semi();
			if (active) {
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.clock.push_back(def);
			}
		} else if (token == "clkpol") {
			if (port.kind == PortKind::Ar) {
				log_error("%s:%d: `clkpol` not allowed in async read port.\n", filename.c_str(), line_number);
			}
			ClkPolDef def;
			token = peek_token();
			if (token == "any") {
				def.kind = ClkPolKind::Any;
				get_token();
			} else if (token == "posedge") {
				def.kind = ClkPolKind::Posedge;
				get_token();
			} else if (token == "negedge") {
				def.kind = ClkPolKind::Negedge;
				get_token();
			} else {
				def.kind = ClkPolKind::Named;
				def.name = get_string();
			}
			get_semi();
			if (active) {
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.clkpol.push_back(def);
			}
		} else if (token == "width") {
			do {
				IntDef def;
				def.val = get_int();
				if (active) {
					def.opts = get_options();
					def.portopts = get_portoptions();
					port.width.push_back(def);
				}
			} while (peek_int());
			get_semi();
		} else if (token == "mixwidth") {
			get_semi();
			if (active) {
				VoidDef def;
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.mixwidth.push_back(def);
			}
		} else if (token == "addrce") {
			get_semi();
			if (active) {
				VoidDef def;
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.addrce.push_back(def);
			}
		} else if (token == "rden") {
			if (port.kind != PortKind::Sr && port.kind != PortKind::Srsw)
				log_error("%s:%d: `rden` only allowed on sync read ports.\n", filename.c_str(), line_number);
			token = get_token();
			RdEnDef def;
			if (token == "none") {
				def.kind = RdEnKind::None;
			} else if (token == "any") {
				def.kind = RdEnKind::Any;
			} else if (token == "write-implies") {
				if (port.kind != PortKind::Srsw)
					log_error("%s:%d: `write-implies` only makes sense for read+write ports.\n", filename.c_str(), line_number);
				def.kind = RdEnKind::WriteImplies;
			} else if (token == "write-excludes") {
				if (port.kind != PortKind::Srsw)
					log_error("%s:%d: `write-excludes` only makes sense for read+write ports.\n", filename.c_str(), line_number);
				def.kind = RdEnKind::WriteExcludes;
			} else {
				log_error("%s:%d: expected `none`, `any`, `write-implies`, or `write-excludes`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			get_semi();
			if (active) {
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.rden.push_back(def);
			}
		} else if (token == "rdinitval" || token == "rdsrstval" || token == "rdarstval") {
			if (port.kind != PortKind::Sr && port.kind != PortKind::Srsw)
				log_error("%s:%d: `%s` only allowed on sync read ports.\n", filename.c_str(), line_number, token.c_str());
			ResetValDef def;
			if (token == "rdinitval")
				def.kind = ResetKind::Init;
			else if (token == "rdsrstval")
				def.kind = ResetKind::Sync;
			else if (token == "rdarstval")
				def.kind = ResetKind::Async;
			else
				abort();
			token = peek_token();
			if (token == "none") {
				def.val_kind = ResetValKind::None;
				get_token();
			} else if (token == "zero") {
				def.val_kind = ResetValKind::Zero;
				get_token();
			} else {
				def.val_kind = ResetValKind::Named;
				def.name = get_string();
			}
			get_semi();
			if (active) {
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.rdrstval.push_back(def);
			}
		} else if (token == "rdsrstmode") {
			if (port.kind != PortKind::Sr && port.kind != PortKind::Srsw)
				log_error("%s:%d: `rdsrstmode` only allowed on sync read ports.\n", filename.c_str(), line_number);
			SrstModeDef def;
			token = get_token();
			if (token == "en-over-srst") {
				def.kind = SrstKind::EnOverSrst;
			} else if (token == "srst-over-en") {
				def.kind = SrstKind::SrstOverEn;
			} else if (token == "any") {
				def.kind = SrstKind::Any;
			} else {
				log_error("%s:%d: expected `en-over-srst`, `srst-over-en`, or `any`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			get_semi();
			if (active) {
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.rdsrstmode.push_back(def);
			}
		} else if (token == "wrbe") {
			if (port.kind == PortKind::Ar || port.kind == PortKind::Sr)
				log_error("%s:%d: `wrbe` only allowed on write ports.\n", filename.c_str(), line_number);
			IntDef def;
			def.val = get_int();
			get_semi();
			if (active) {
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.wrbe.push_back(def);
			}
		} else if (token == "wrprio") {
			if (port.kind == PortKind::Ar || port.kind == PortKind::Sr)
				log_error("%s:%d: `wrprio` only allowed on write ports.\n", filename.c_str(), line_number);
			do {
				StringDef def;
				def.val = get_string();
				if (active) {
					def.opts = get_options();
					def.portopts = get_portoptions();
					port.wrprio.push_back(def);
				}
			} while (peek_string());
			get_semi();
		} else if (token == "wrtrans") {
			if (port.kind == PortKind::Ar || port.kind == PortKind::Sr)
				log_error("%s:%d: `wrtrans` only allowed on write ports.\n", filename.c_str(), line_number);
			token = peek_token();
			WrTransDef def;
			if (token == "self") {
				if (port.kind != PortKind::Srsw)
					log_error("%s:%d: `wrtrans self` only allowed on sync read + sync write ports.\n", filename.c_str(), line_number);
				def.target_kind = TransTargetKind::Self;
				get_token();
			} else if (token == "other") {
				def.target_kind = TransTargetKind::Other;
				get_token();
			} else {
				def.target_kind = TransTargetKind::Named;
				def.target_name = get_string();
			}
			token = get_token();
			if (token == "new") {
				def.kind = TransKind::New;
			} else if (token == "new-but-be") {
				def.kind = TransKind::NewButBe;
			} else if (token == "old") {
				def.kind = TransKind::Old;
			} else {
				log_error("%s:%d: expected `new`, `new-but-be`, or `old`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			get_semi();
			if (active) {
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.wrtrans.push_back(def);
			}
		} else if (token == "wrcs") {
			if (port.kind == PortKind::Ar || port.kind == PortKind::Sr)
				log_error("%s:%d: `wrcs` only allowed on write ports.\n", filename.c_str(), line_number);
			IntDef def;
			def.val = get_int();
			get_semi();
			if (active) {
				def.opts = get_options();
				def.portopts = get_portoptions();
				port.wrcs.push_back(def);
			}
		} else if (token == "") {
			log_error("%s:%d: unexpected EOF while parsing port item.\n", filename.c_str(), line_number);
		} else {
			log_error("%s:%d: unknown port-level item `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
	}

	void parse_ram_item() {
		std::string token = get_token();
		if (token == "ifdef") {
			bool save = enter_ifdef(true);
			parse_ram_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_ram_block();
			}
			active = save;
		} else if (token == "ifndef") {
			bool save = enter_ifdef(false);
			parse_ram_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_ram_block();
			}
			active = save;
		} else if (token == "option") {
			enter_option();
			parse_ram_block();
			exit_option();
		} else if (token == "dims") {
			MemoryDimsDef dims;
			dims.abits = get_int();
			dims.dbits = get_int();
			get_semi();
			if (active) {
				dims.opts = get_options();
				ram.dims.push_back(dims);
			}
		} else if (token == "init") {
			MemoryInitDef init;
			token = get_token();
			if (token == "zero") {
				init.kind = MemoryInitKind::Zero;
			} else if (token == "any") {
				init.kind = MemoryInitKind::Any;
			} else if (token == "none") {
				init.kind = MemoryInitKind::None;
			} else {
				log_error("%s:%d: expected `zero`, `any`, or `none`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			get_semi();
			if (active) {
				init.opts = get_options();
				ram.init.push_back(init);
			}
		} else if (token == "style") {
			do {
				RamStringDef def;
				def.val = get_string();
				if (active) {
					def.opts = get_options();
					ram.style.push_back(def);
				}
			} while (peek_string());
			get_semi();
		} else if (token == "port") {
			int orig_line = line_number;
			port = PortGroupDef();
			token = get_token();
			if (token == "ar") {
				port.kind = PortKind::Ar;
			} else if (token == "sr") {
				port.kind = PortKind::Sr;
			} else if (token == "sw") {
				port.kind = PortKind::Sw;
			} else if (token == "arsw") {
				port.kind = PortKind::Arsw;
			} else if (token == "srsw") {
				port.kind = PortKind::Srsw;
			} else {
				log_error("%s:%d: expected `ar`, `sr`, `sw`, `arsw`, or `srsw`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			do {
				port.names.push_back(get_string());
			} while (peek_string());
			parse_port_block();
			if (active) {
				// Add defaults for some options.
				if (port.kind != PortKind::Ar) {
					if (port.clock.empty()) {
						StringDef def;
						port.clock.push_back(def);
					}
					if (port.clkpol.empty()) {
						ClkPolDef def;
						def.kind = ClkPolKind::Any;
						port.clkpol.push_back(def);
					}
				}
				if (port.width.empty()) {
					IntDef def;
					def.val = 1;
					port.width.push_back(def);
				}
				// Refuse to guess this one — there is no "safe" default.
				if (port.kind == PortKind::Sr || port.kind == PortKind::Srsw) {
					if (port.rden.empty()) {
						log_error("%s:%d: `rden` capability should be specified.\n", filename.c_str(), orig_line);
					}
				}
				port.opts = get_options();
				ram.ports.push_back(port);
			}
		} else if (token == "") {
			log_error("%s:%d: unexpected EOF while parsing ram item.\n", filename.c_str(), line_number);
		} else {
			log_error("%s:%d: unknown ram-level item `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
	}

	void parse_top_item() {
		std::string token = get_token();
		if (token == "ifdef") {
			bool save = enter_ifdef(true);
			parse_top_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_top_block();
			}
			active = save;
		} else if (token == "ifndef") {
			bool save = enter_ifdef(false);
			parse_top_block();
			if (peek_token() == "else") {
				enter_else(save);
				parse_top_block();
			}
			active = save;
		} else if (token == "ram") {
			int orig_line = line_number;
			ram = RamDef();
			token = get_token();
			if (token == "distributed") {
				ram.kind = RamKind::Distributed;
			} else if (token == "block") {
				ram.kind = RamKind::Block;
			} else if (token == "huge") {
				ram.kind = RamKind::Huge;
			} else {
				log_error("%s:%d: expected `distributed`, `block`, or `huge`, got `%s`.\n", filename.c_str(), line_number, token.c_str());
			}
			ram.id = get_id();
			parse_ram_block();
			if (active) {
				if (ram.dims.empty())
					log_error("%s:%d: `dims` capability should be specified.\n", filename.c_str(), orig_line);
				if (ram.ports.empty())
					log_error("%s:%d: at least one port group should be specified.\n", filename.c_str(), orig_line);
				lib.ram_defs.push_back(ram);
			}
		} else if (token == "") {
			log_error("%s:%d: unexpected EOF while parsing top item.\n", filename.c_str(), line_number);
		} else {
			log_error("%s:%d: unknown top-level item `%s`.\n", filename.c_str(), line_number, token.c_str());
		}
	}

	void parse() {
		while (peek_token() != "")
			parse_top_item();
	}
};

struct MemoryLibMapPass : public Pass {
	MemoryLibMapPass() : Pass("memory_libmap", "map memories to cells") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    memory_libmap -lib <library_file> [-D <condition>] [selection]\n");
		log("\n");
		log("This pass takes a description of available RAM cell types and maps\n");
		log("all selected memories to one of them, or leaves them  to be mapped to FFs.\n");
		log("\n");
		log("  -lib <library_file>\n");
		log("    Selects a library file containing RAM cell definitions. This option\n");
		log("    can be passed more than once to select multiple libraries.\n");
		log("\n");
		log("  -D <condition>\n");
		log("    Enables a condition that can be checked within the library file\n");
		log("    to eg. select between slightly different hardware variants.\n");
		log("    This option can be passed any number of times.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		std::vector<std::string> lib_files;
		pool<std::string> defines;
		log_header(design, "Executing MEMORY_LIBMAP pass (mapping memories to cells).\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-lib" && argidx+1 < args.size()) {
				lib_files.push_back(args[++argidx]);
				continue;
			}
			if (args[argidx] == "-D" && argidx+1 < args.size()) {
				defines.insert(args[++argidx]);
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		Library lib(defines);
		for (auto &file: lib_files) {
			Parser(file, lib);
		}
		lib.prepare();

		for (auto module : design->selected_modules()) {
			for (auto &mem : Mem::get_selected_memories(module))
			{
				// TODO
			}
		}
	}
} MemoryLibMapPass;

PRIVATE_NAMESPACE_END
