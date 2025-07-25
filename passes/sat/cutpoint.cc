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
#include "kernel/log_help.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct CutpointPass : public Pass {
	CutpointPass() : Pass("cutpoint", "adds formal cut points to the design") { }
	bool formatted_help() override {
		auto *help = PrettyHelp::get_current();
		help->set_group("formal");
		return false;
	}
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    cutpoint [options] [selection]\n");
		log("\n");
		log("This command adds formal cut points to the design.\n");
		log("\n");
		log("    -undef\n");
		log("        set cutpoint nets to undef (x). the default behavior is to create\n");
		log("        an $anyseq cell and drive the cutpoint net from that\n");
		log("\n");
		log("    -noscopeinfo\n");
		log("        do not create '$scopeinfo' cells that preserve attributes of cells that\n");
		log("        were removed by this pass\n");
		log("\n");
		log("    cutpoint -blackbox [options]\n");
		log("\n");
		log("Replace all instances of blackboxes in the design with a formal cut point.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		bool flag_undef = false;
		bool flag_scopeinfo = true;
		bool flag_blackbox = false;

		log_header(design, "Executing CUTPOINT pass.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "-undef") {
				flag_undef = true;
				continue;
			}
			if (args[argidx] == "-noscopeinfo") {
				flag_scopeinfo = false;
				continue;
			}
			if (args[argidx] == "-blackbox") {
				flag_blackbox = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (flag_blackbox) {
			if (!design->full_selection())
				log_cmd_error("This command only operates on fully selected designs!\n");
			design->push_empty_selection();
			auto &selection = design->selection();
			for (auto module : design->modules())
				for (auto cell : module->cells())
					if (selection.boxed_module(cell->type))
						selection.select(module, cell);
		}

		for (auto module : design->all_selected_modules())
		{
			if (module->is_selected_whole()) {
				log("Making all outputs of module %s cut points, removing module contents.\n", log_id(module));
				module->new_connections(std::vector<RTLIL::SigSig>());
				for (auto cell : vector<Cell*>(module->cells()))
					module->remove(cell);
				vector<Wire*> output_wires;
				for (auto wire : module->wires())
					if (wire->port_output)
						output_wires.push_back(wire);
				for (auto wire : output_wires)
					module->connect(wire, flag_undef ? Const(State::Sx, GetSize(wire)) : module->Anyseq(NEW_ID, GetSize(wire)));
				continue;
			}

			SigMap sigmap(module);
			pool<SigBit> cutpoint_bits;

			for (auto cell : module->selected_cells()) {
				if (cell->type == ID($anyseq))
					continue;
				log("Removing cell %s.%s, making all cell outputs cutpoints.\n", log_id(module), log_id(cell));
				for (auto &conn : cell->connections()) {
					if (cell->output(conn.first))
						module->connect(conn.second, flag_undef ? Const(State::Sx, GetSize(conn.second)) : module->Anyseq(NEW_ID, GetSize(conn.second)));
				}

				RTLIL::Cell *scopeinfo = nullptr;
				auto cell_name = cell->name;
				if (flag_scopeinfo && cell_name.isPublic()) {
					auto scopeinfo = module->addCell(NEW_ID, ID($scopeinfo));
					scopeinfo->setParam(ID::TYPE, RTLIL::Const("blackbox"));

					for (auto const &attr : cell->attributes)
					{
						if (attr.first == ID::hdlname)
							scopeinfo->attributes.insert(attr);
						else
							scopeinfo->attributes.emplace(stringf("\\cell_%s", RTLIL::unescape_id(attr.first).c_str()), attr.second);
					}
				}

				module->remove(cell);

				if (scopeinfo != nullptr)
					module->rename(scopeinfo, cell_name);
			}

			for (auto wire : module->selected_wires()) {
				if (wire->port_output) {
					log("Making output wire %s.%s a cutpoint.\n", log_id(module), log_id(wire));
					Wire *new_wire = module->addWire(NEW_ID, wire);
					module->swap_names(wire, new_wire);
					module->connect(new_wire, flag_undef ? Const(State::Sx, GetSize(new_wire)) : module->Anyseq(NEW_ID, GetSize(new_wire)));
					wire->port_id = 0;
					wire->port_input = false;
					wire->port_output = false;
					continue;
				}
				log("Making wire %s.%s a cutpoint.\n", log_id(module), log_id(wire));
				for (auto bit : sigmap(wire))
					cutpoint_bits.insert(bit);
			}

			if (!cutpoint_bits.empty())
			{
				for (auto cell : module->cells()) {
					for (auto &conn : cell->connections()) {
						if (!cell->output(conn.first))
							continue;
						SigSpec sig = sigmap(conn.second);
						int bit_count = 0;
						for (auto &bit : sig) {
							if (cutpoint_bits.count(bit))
								bit_count++;
						}
						if (bit_count == 0)
							continue;
						SigSpec dummy = module->addWire(NEW_ID, bit_count);
						bit_count = 0;
						for (auto &bit : sig) {
							if (cutpoint_bits.count(bit))
								bit = dummy[bit_count++];
						}
						cell->setPort(conn.first, sig);
					}
				}

				vector<Wire*> rewrite_wires;
				for (auto id : module->ports) {
					RTLIL::Wire *wire = module->wire(id);
					if (wire->port_input) {
						int bit_count = 0;
						for (auto &bit : sigmap(wire))
							if (cutpoint_bits.count(bit))
								bit_count++;
						if (bit_count)
							rewrite_wires.push_back(wire);
					}
				}

				for (auto wire : rewrite_wires) {
					Wire *new_wire = module->addWire(NEW_ID, wire);
					SigSpec lhs, rhs, sig = sigmap(wire);
					for (int i = 0; i < GetSize(sig); i++)
						if (!cutpoint_bits.count(sig[i])) {
							lhs.append(SigBit(wire, i));
							rhs.append(SigBit(new_wire, i));
						}
					if (GetSize(lhs))
						module->connect(lhs, rhs);
					module->swap_names(wire, new_wire);
					wire->port_id = 0;
					wire->port_input = false;
					wire->port_output = false;
				}

				SigSpec sig(cutpoint_bits);
				sig.sort_and_unify();

				for (auto chunk : sig.chunks()) {
					SigSpec s(chunk);
					module->connect(s, flag_undef ? Const(State::Sx, GetSize(s)) : module->Anyseq(NEW_ID, GetSize(s)));
				}
			}
		}
	}
} CutpointPass;

PRIVATE_NAMESPACE_END
