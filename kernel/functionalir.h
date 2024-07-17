/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2024  Emily Schmidt <emily@yosyshq.com>
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

#ifndef FUNCTIONALIR_H
#define FUNCTIONALIR_H

#include "kernel/yosys.h"
#include "kernel/functional.h"
#include "kernel/drivertools.h"
#include "kernel/mem.h"
#include "kernel/topo_scc.h"

USING_YOSYS_NAMESPACE
YOSYS_NAMESPACE_BEGIN

class FunctionalIR {
public:
	// each function is documented with a short pseudocode declaration or definition
	// standard C/Verilog operators are used to describe the result
	// 
	// the types used in this are:
	// - bit[N]: a bitvector of N bits
	//   bit[N] can be indicated as signed or unsigned. this is not tracked by the functional backend
	//   but is meant to indicate how the value is interpreted
	//   if a bit[N] is marked as neither signed nor unsigned, this means the result should be valid with *either* interpretation
	// - memory[N, M]: a memory with N address and M data bits
	// - int: C++ int
	// - Const[N]: yosys RTLIL::Const (with size() == N)
	// - IdString: yosys IdString
	// - any: used in documentation to indicate that the type is unconstrained
	//
	// nodes in the functional backend are either of type bit[N] or memory[N,M] (for some N, M: int)
	// additionally, they can carry a constant of type int, Const[N] or IdString
	// each node has a 'sort' field that stores the type of the node
	// slice, zero_extend, sign_extend use the type field to store out_width
	enum class Fn {
		// invalid() = known-invalid/shouldn't happen value
		// TODO: maybe remove this and use e.g. std::optional instead?
		invalid,
		// buf(a: any): any = a
		// no-op operation
		// when constructing the compute graph we generate invalid buf() nodes as a placeholder
		// and later insert the argument
		buf,
		// slice(a: bit[in_width], offset: int, out_width: int): bit[out_width] = a[offset +: out_width]
		// required: offset + out_width <= in_width
		slice,
		// zero_extend(a: unsigned bit[in_width], out_width: int): unsigned bit[out_width] = a (zero extended)
		// required: out_width > in_width
		zero_extend,
		// sign_extend(a: signed bit[in_width], out_width: int): signed bit[out_width] = a (sign extended)
		// required: out_width > in_width
		sign_extend,
		// concat(a: bit[N], b: bit[M]): bit[N+M] = {b, a} (verilog syntax)
		// concatenates two bitvectors, with a in the least significant position and b in the more significant position
		concat,
		// add(a: bit[N], b: bit[N]): bit[N] = a + b
		add,
		// sub(a: bit[N], b: bit[N]): bit[N] = a - b
		sub,
		// mul(a: bit[N], b: bit[N]): bit[N] = a * b
		mul,
		// unsigned_div(a: unsigned bit[N], b: unsigned bit[N]): bit[N] = a / b
		unsigned_div,
		// unsigned_mod(a: signed bit[N], b: signed bit[N]): bit[N] = a % b
		unsigned_mod,
		// bitwise_and(a: bit[N], b: bit[N]): bit[N] = a & b
		bitwise_and,
		// bitwise_or(a: bit[N], b: bit[N]): bit[N] = a | b
		bitwise_or,
		// bitwise_xor(a: bit[N], b: bit[N]): bit[N] = a ^ b
		bitwise_xor,
		// bitwise_not(a: bit[N]): bit[N] = ~a
		bitwise_not,
		// reduce_and(a: bit[N]): bit[1] = &a
		reduce_and,
		// reduce_or(a: bit[N]): bit[1] = |a
		reduce_or,
		// reduce_xor(a: bit[N]): bit[1] = ^a
		reduce_xor,
		// unary_minus(a: bit[N]): bit[N] = -a
		unary_minus,
		// equal(a: bit[N], b: bit[N]): bit[1] = (a == b)
		equal,
		// not_equal(a: bit[N], b: bit[N]): bit[1] = (a != b)
		not_equal,
		// signed_greater_than(a: signed bit[N], b: signed bit[N]): bit[1] = (a > b)
		signed_greater_than,
		// signed_greater_equal(a: signed bit[N], b: signed bit[N]): bit[1] = (a >= b)
		signed_greater_equal,
		// unsigned_greater_than(a: unsigned bit[N], b: unsigned bit[N]): bit[1] = (a > b)
		unsigned_greater_than,
		// unsigned_greater_equal(a: unsigned bit[N], b: unsigned bit[N]): bit[1] = (a >= b)
		unsigned_greater_equal,
		// logical_shift_left(a: bit[N], b: unsigned bit[M]): bit[N] = a << b
		// required: M == clog2(N)
		logical_shift_left,
		// logical_shift_right(a: unsigned bit[N], b: unsigned bit[M]): unsigned bit[N] = a >> b
		// required: M == clog2(N)
		logical_shift_right,
		// arithmetic_shift_right(a: signed bit[N], b: unsigned bit[M]): signed bit[N] = a >> b
		// required: M == clog2(N)
		arithmetic_shift_right,
		// mux(a: bit[N], b: bit[N], s: bit[1]): bit[N] = s ? b : a
		mux,
		// constant(a: Const[N]): bit[N] = a
		constant,
		// input(a: IdString): any
		// returns the current value of the input with the specified name
		input,
		// state(a: IdString): any
		// returns the current value of the state variable with the specified name
		state,
		// multiple(a: any, b: any, c: any, ...): any
		// indicates a value driven by multiple inputs
		multiple,
		// undriven(width: int): bit[width]
		// indicates an undriven value
		undriven,
		// memory_read(memory: memory[addr_width, data_width], addr: bit[addr_width]): bit[data_width] = memory[addr]
		memory_read,
		// memory_write(memory: memory[addr_width, data_width], addr: bit[addr_width], data: bit[data_width]): memory[addr_width, data_width]
		// returns a copy of `memory` but with the value at `addr` changed to `data`
		memory_write
	};
	// returns the name of a FunctionalIR::Fn value, as a string literal
	static const char *fn_to_string(Fn);
	// FunctionalIR::Sort represents the sort or type of a node
	// currently the only two types are signal/bit and memory
	class Sort {
		std::variant<int, std::pair<int, int>> _v;
	public:
		explicit Sort(int width) : _v(width) { }
		Sort(int addr_width, int data_width) : _v(std::make_pair(addr_width, data_width)) { }
		bool is_signal() const { return _v.index() == 0; }
		bool is_memory() const { return _v.index() == 1; }
		// returns the width of a bitvector type, errors out for other types
		int width() const { return std::get<0>(_v); }
		// returns the address width of a bitvector type, errors out for other types
		int addr_width() const { return std::get<1>(_v).first; }
		// returns the data width of a bitvector type, errors out for other types
		int data_width() const { return std::get<1>(_v).second; }
		bool operator==(Sort const& other) const { return _v == other._v; }
		unsigned int hash() const { return mkhash(_v); }
	};
private:
	// one NodeData is stored per Node, containing the function and non-node arguments
	// note that NodeData is deduplicated by ComputeGraph
	class NodeData {
		Fn _fn;
		std::variant<
			std::monostate,
			RTLIL::Const,
			IdString,
			int
		> _extra;
	public:
		NodeData() : _fn(Fn::invalid) {}
		NodeData(Fn fn) : _fn(fn) {}
		template<class T> NodeData(Fn fn, T &&extra) : _fn(fn), _extra(std::forward<T>(extra)) {}
		Fn fn() const { return _fn; }
		const RTLIL::Const &as_const() const { return std::get<RTLIL::Const>(_extra); }
		IdString as_idstring() const { return std::get<IdString>(_extra); }
		int as_int() const { return std::get<int>(_extra); }
		int hash() const {
			return mkhash((unsigned int) _fn, mkhash(_extra));
		}
		bool operator==(NodeData const &other) const {
			return _fn == other._fn && _extra == other._extra;
		}
	};
	// Attr contains all the information about a note that should not be deduplicated
	struct Attr {
		Sort sort;
	};
	// our specialised version of ComputeGraph
	// the sparse_attr IdString stores a naming suggestion, retrieved with name()
	// the key is currently used to identify the nodes that represent output and next state values
	// the bool is true for next state values
	using Graph = ComputeGraph<NodeData, Attr, IdString, std::pair<IdString, bool>>;
	Graph _graph;
	dict<IdString, Sort> _inputs;
	dict<IdString, Sort> _outputs;
	dict<IdString, Sort> _state;
	void add_input(IdString name, Sort sort) {
		auto [it, found] = _inputs.emplace(name, std::move(sort));
		if(found)
			log_assert(it->second == sort);
	}
	void add_state(IdString name, Sort sort) {
		auto [it, found] = _state.emplace(name, std::move(sort));
		if(found)
			log_assert(it->second == sort);
	}
	void add_output(IdString name, Sort sort) {
		auto [it, found] = _outputs.emplace(name, std::move(sort));
		if(found)
			log_assert(it->second == sort);
	}
public:
	class Factory;
	// Node is an immutable reference to a FunctionalIR node
	class Node {
		friend class Factory;
		friend class FunctionalIR;
		Graph::ConstRef _ref;
		explicit Node(Graph::ConstRef ref) : _ref(ref) { }
		explicit operator Graph::ConstRef() { return _ref; }
	public:
		// the node's index. may change if nodes are added or removed
		int id() const { return _ref.index(); }
		// a name suggestion for the node, which need not be unique
		IdString name() const {
			if(_ref.has_sparse_attr())
				return _ref.sparse_attr();
			else
				return std::string("\\n") + std::to_string(id());
		}
		Fn fn() const { return _ref.function().fn(); }
		Sort sort() const { return _ref.attr().sort; }
		// returns the width of a bitvector node, errors out for other nodes
		int width() const { return sort().width(); }
		size_t arg_count() const { return _ref.size(); }
		Node arg(int n) const { return Node(_ref.arg(n)); }
		// visit calls the appropriate visitor method depending on the type of the node
		template<class Visitor> auto visit(Visitor v) const
		{
			// currently templated but could be switched to AbstractVisitor &
			switch(_ref.function().fn()) {
			case Fn::invalid: log_error("invalid node in visit"); break;
			case Fn::buf: return v.buf(*this, arg(0)); break;
			case Fn::slice: return v.slice(*this, arg(0), _ref.function().as_int(), sort().width()); break;
			case Fn::zero_extend: return v.zero_extend(*this, arg(0), width()); break;
			case Fn::sign_extend: return v.sign_extend(*this, arg(0), width()); break;
			case Fn::concat: return v.concat(*this, arg(0), arg(1)); break;
			case Fn::add: return v.add(*this, arg(0), arg(1)); break;
			case Fn::sub: return v.sub(*this, arg(0), arg(1)); break;
			case Fn::mul: return v.mul(*this, arg(0), arg(1)); break;
			case Fn::unsigned_div: return v.unsigned_div(*this, arg(0), arg(1)); break;
			case Fn::unsigned_mod: return v.unsigned_mod(*this, arg(0), arg(1)); break;
			case Fn::bitwise_and: return v.bitwise_and(*this, arg(0), arg(1)); break;
			case Fn::bitwise_or: return v.bitwise_or(*this, arg(0), arg(1)); break;
			case Fn::bitwise_xor: return v.bitwise_xor(*this, arg(0), arg(1)); break;
			case Fn::bitwise_not: return v.bitwise_not(*this, arg(0)); break;
			case Fn::unary_minus: return v.unary_minus(*this, arg(0)); break;
			case Fn::reduce_and: return v.reduce_and(*this, arg(0)); break;
			case Fn::reduce_or: return v.reduce_or(*this, arg(0)); break;
			case Fn::reduce_xor: return v.reduce_xor(*this, arg(0)); break;
			case Fn::equal: return v.equal(*this, arg(0), arg(1)); break;
			case Fn::not_equal: return v.not_equal(*this, arg(0), arg(1)); break;
			case Fn::signed_greater_than: return v.signed_greater_than(*this, arg(0), arg(1)); break; 
			case Fn::signed_greater_equal: return v.signed_greater_equal(*this, arg(0), arg(1)); break;
			case Fn::unsigned_greater_than: return v.unsigned_greater_than(*this, arg(0), arg(1)); break; 
			case Fn::unsigned_greater_equal: return v.unsigned_greater_equal(*this, arg(0), arg(1)); break;
			case Fn::logical_shift_left: return v.logical_shift_left(*this, arg(0), arg(1)); break;
			case Fn::logical_shift_right: return v.logical_shift_right(*this, arg(0), arg(1)); break;
			case Fn::arithmetic_shift_right: return v.arithmetic_shift_right(*this, arg(0), arg(1)); break;
			case Fn::mux: return v.mux(*this, arg(0), arg(1), arg(2)); break;
			case Fn::constant: return v.constant(*this, _ref.function().as_const()); break;
			case Fn::input: return v.input(*this, _ref.function().as_idstring()); break;
			case Fn::state: return v.state(*this, _ref.function().as_idstring()); break;
			case Fn::memory_read: return v.memory_read(*this, arg(0), arg(1)); break;
			case Fn::memory_write: return v.memory_write(*this, arg(0), arg(1), arg(2)); break;
			case Fn::multiple: log_error("multiple in visit"); break;
			case Fn::undriven: return v.undriven(*this, width()); break;
			}
		}
		std::string to_string();
		std::string to_string(std::function<std::string(Node)>);
	};
	// AbstractVisitor provides an abstract base class for visitors
	template<class T> struct AbstractVisitor {
		virtual T buf(Node self, Node n) = 0;
		virtual T slice(Node self, Node a, int offset, int out_width) = 0;
		virtual T zero_extend(Node self, Node a, int out_width) = 0;
		virtual T sign_extend(Node self, Node a, int out_width) = 0;
		virtual T concat(Node self, Node a, Node b) = 0;
		virtual T add(Node self, Node a, Node b) = 0;
		virtual T sub(Node self, Node a, Node b) = 0;
		virtual T mul(Node self, Node a, Node b) = 0;
		virtual T unsigned_div(Node self, Node a, Node b) = 0;
		virtual T unsigned_mod(Node self, Node a, Node b) = 0;
		virtual T bitwise_and(Node self, Node a, Node b) = 0;
		virtual T bitwise_or(Node self, Node a, Node b) = 0;
		virtual T bitwise_xor(Node self, Node a, Node b) = 0;
		virtual T bitwise_not(Node self, Node a) = 0;
		virtual T unary_minus(Node self, Node a) = 0;
		virtual T reduce_and(Node self, Node a) = 0;
		virtual T reduce_or(Node self, Node a) = 0;
		virtual T reduce_xor(Node self, Node a) = 0;
		virtual T equal(Node self, Node a, Node b) = 0;
		virtual T not_equal(Node self, Node a, Node b) = 0;
		virtual T signed_greater_than(Node self, Node a, Node b) = 0;
		virtual T signed_greater_equal(Node self, Node a, Node b) = 0;
		virtual T unsigned_greater_than(Node self, Node a, Node b) = 0;
		virtual T unsigned_greater_equal(Node self, Node a, Node b) = 0;
		virtual T logical_shift_left(Node self, Node a, Node b) = 0;
		virtual T logical_shift_right(Node self, Node a, Node b) = 0;
		virtual T arithmetic_shift_right(Node self, Node a, Node b) = 0;
		virtual T mux(Node self, Node a, Node b, Node s) = 0;
		virtual T constant(Node self, RTLIL::Const value) = 0;
		virtual T input(Node self, IdString name) = 0;
		virtual T state(Node self, IdString name) = 0;
		virtual T memory_read(Node self, Node mem, Node addr) = 0;
		virtual T memory_write(Node self, Node mem, Node addr, Node data) = 0;
		virtual T undriven(Node self, int width) = 0;
	};
	// DefaultVisitor provides defaults for all visitor methods which just calls default_handler
	template<class T> struct DefaultVisitor : public AbstractVisitor<T> {
		virtual T default_handler(Node self) = 0;
		T buf(Node self, Node) override { return default_handler(self); }
		T slice(Node self, Node, int, int) override { return default_handler(self); }
		T zero_extend(Node self, Node, int) override { return default_handler(self); }
		T sign_extend(Node self, Node, int) override { return default_handler(self); }
		T concat(Node self, Node, Node) override { return default_handler(self); }
		T add(Node self, Node, Node) override { return default_handler(self); }
		T sub(Node self, Node, Node) override { return default_handler(self); }
		T mul(Node self, Node, Node) override { return default_handler(self); }
		T unsigned_div(Node self, Node, Node) override { return default_handler(self); }
		T unsigned_mod(Node self, Node, Node) override { return default_handler(self); }
		T bitwise_and(Node self, Node, Node) override { return default_handler(self); }
		T bitwise_or(Node self, Node, Node) override { return default_handler(self); }
		T bitwise_xor(Node self, Node, Node) override { return default_handler(self); }
		T bitwise_not(Node self, Node) override { return default_handler(self); }
		T unary_minus(Node self, Node) override { return default_handler(self); }
		T reduce_and(Node self, Node) override { return default_handler(self); }
		T reduce_or(Node self, Node) override { return default_handler(self); }
		T reduce_xor(Node self, Node) override { return default_handler(self); }
		T equal(Node self, Node, Node) override { return default_handler(self); }
		T not_equal(Node self, Node, Node) override { return default_handler(self); }
		T signed_greater_than(Node self, Node, Node) override { return default_handler(self); }
		T signed_greater_equal(Node self, Node, Node) override { return default_handler(self); }
		T unsigned_greater_than(Node self, Node, Node) override { return default_handler(self); }
		T unsigned_greater_equal(Node self, Node, Node) override { return default_handler(self); }
		T logical_shift_left(Node self, Node, Node) override { return default_handler(self); }
		T logical_shift_right(Node self, Node, Node) override { return default_handler(self); }
		T arithmetic_shift_right(Node self, Node, Node) override { return default_handler(self); }
		T mux(Node self, Node, Node, Node) override { return default_handler(self); }
		T constant(Node self, RTLIL::Const) override { return default_handler(self); }
		T input(Node self, IdString) override { return default_handler(self); }
		T state(Node self, IdString) override { return default_handler(self); }
		T memory_read(Node self, Node, Node) override { return default_handler(self); }
		T memory_write(Node self, Node, Node, Node) override { return default_handler(self); }
		T undriven(Node self, int) override { return default_handler(self); }
	};
	// a factory is used to modify a FunctionalIR. it creates new nodes and allows for some modification of existing nodes.
	class Factory {
		FunctionalIR &_ir;
		friend class FunctionalIR;
		explicit Factory(FunctionalIR &ir) : _ir(ir) {}
		Node add(NodeData &&fn, Sort &&sort, std::initializer_list<Node> args) {
			log_assert(!sort.is_signal() || sort.width() > 0);
			log_assert(!sort.is_memory() || sort.addr_width() > 0 && sort.data_width() > 0);
			Graph::Ref ref = _ir._graph.add(std::move(fn), {std::move(sort)});
			for (auto arg : args)
				ref.append_arg(Graph::ConstRef(arg));
			return Node(ref);
		}
		Graph::Ref mutate(Node n) {
			return _ir._graph[n._ref.index()];
		}
		void check_basic_binary(Node const &a, Node const &b) { log_assert(a.sort().is_signal() && a.sort() == b.sort()); }
		void check_shift(Node const &a, Node const &b) { log_assert(a.sort().is_signal() && b.sort().is_signal() && b.width() == ceil_log2(a.width())); }
		void check_unary(Node const &a) { log_assert(a.sort().is_signal()); }
	public:
		Node slice(Node a, int offset, int out_width) {
			log_assert(a.sort().is_signal() && offset + out_width <= a.sort().width());
			if(offset == 0 && out_width == a.width())
				return a;
			return add(NodeData(Fn::slice, offset), Sort(out_width), {a});
		}
		// extend will either extend or truncate the provided value to reach the desired width
		Node extend(Node a, int out_width, bool is_signed) {
			int in_width = a.sort().width();
			log_assert(a.sort().is_signal());
			if(in_width == out_width)
				return a;
			if(in_width > out_width)
				return slice(a, 0, out_width);
			if(is_signed)
				return add(Fn::sign_extend, Sort(out_width), {a});
			else
				return add(Fn::zero_extend, Sort(out_width), {a});
		}
		Node concat(Node a, Node b) {
			log_assert(a.sort().is_signal() && b.sort().is_signal());
			return add(Fn::concat, Sort(a.sort().width() + b.sort().width()), {a, b});
		}
		Node add(Node a, Node b) { check_basic_binary(a, b); return add(Fn::add, a.sort(), {a, b}); }
		Node sub(Node a, Node b) { check_basic_binary(a, b); return add(Fn::sub, a.sort(), {a, b}); }
		Node mul(Node a, Node b) { check_basic_binary(a, b); return add(Fn::mul, a.sort(), {a, b}); }
		Node unsigned_div(Node a, Node b) { check_basic_binary(a, b); return add(Fn::unsigned_div, a.sort(), {a, b}); }
		Node unsigned_mod(Node a, Node b) { check_basic_binary(a, b); return add(Fn::unsigned_mod, a.sort(), {a, b}); }
		Node bitwise_and(Node a, Node b) { check_basic_binary(a, b); return add(Fn::bitwise_and, a.sort(), {a, b}); }
		Node bitwise_or(Node a, Node b) { check_basic_binary(a, b); return add(Fn::bitwise_or, a.sort(), {a, b}); }
		Node bitwise_xor(Node a, Node b) { check_basic_binary(a, b); return add(Fn::bitwise_xor, a.sort(), {a, b}); }
		Node bitwise_not(Node a) { check_unary(a); return add(Fn::bitwise_not, a.sort(), {a}); }
		Node unary_minus(Node a) { check_unary(a); return add(Fn::unary_minus, a.sort(), {a}); }
		Node reduce_and(Node a) {
			check_unary(a);
			if(a.width() == 1)
				return a;
			return add(Fn::reduce_and, Sort(1), {a});
		}
		Node reduce_or(Node a) {
			check_unary(a);
			if(a.width() == 1)
				return a;
			return add(Fn::reduce_or, Sort(1), {a});
		}
		Node reduce_xor(Node a) { 
			check_unary(a);
			if(a.width() == 1)
				return a;
			return add(Fn::reduce_xor, Sort(1), {a});
		}
		Node equal(Node a, Node b) { check_basic_binary(a, b); return add(Fn::equal, Sort(1), {a, b}); }
		Node not_equal(Node a, Node b) { check_basic_binary(a, b); return add(Fn::not_equal, Sort(1), {a, b}); }
		Node signed_greater_than(Node a, Node b) { check_basic_binary(a, b); return add(Fn::signed_greater_than, Sort(1), {a, b}); }
		Node signed_greater_equal(Node a, Node b) { check_basic_binary(a, b); return add(Fn::signed_greater_equal, Sort(1), {a, b}); }
		Node unsigned_greater_than(Node a, Node b) { check_basic_binary(a, b); return add(Fn::unsigned_greater_than, Sort(1), {a, b}); }
		Node unsigned_greater_equal(Node a, Node b) { check_basic_binary(a, b); return add(Fn::unsigned_greater_equal, Sort(1), {a, b}); }
		Node logical_shift_left(Node a, Node b) { check_shift(a, b); return add(Fn::logical_shift_left, a.sort(), {a, b}); }
		Node logical_shift_right(Node a, Node b) { check_shift(a, b); return add(Fn::logical_shift_right, a.sort(), {a, b}); }
		Node arithmetic_shift_right(Node a, Node b) { check_shift(a, b); return add(Fn::arithmetic_shift_right, a.sort(), {a, b}); }
		Node mux(Node a, Node b, Node s) {
			log_assert(a.sort().is_signal() && a.sort() == b.sort() && s.sort() == Sort(1));
			return add(Fn::mux, a.sort(), {a, b, s});
		}
		Node memory_read(Node mem, Node addr) {
			log_assert(mem.sort().is_memory() && addr.sort().is_signal() && mem.sort().addr_width() == addr.sort().width());
			return add(Fn::memory_read, Sort(mem.sort().data_width()), {mem, addr});
		}
		Node memory_write(Node mem, Node addr, Node data) {
			log_assert(mem.sort().is_memory() && addr.sort().is_signal() && data.sort().is_signal() &&
				mem.sort().addr_width() == addr.sort().width() && mem.sort().data_width() == data.sort().width());
			return add(Fn::memory_write, mem.sort(), {mem, addr, data});
		}
		Node constant(RTLIL::Const value) {
			return add(NodeData(Fn::constant, std::move(value)), Sort(value.size()), {});
		}
		Node create_pending(int width) {
			return add(Fn::buf, Sort(width), {});
		}
		void update_pending(Node node, Node value) {
			log_assert(node._ref.function() == Fn::buf && node._ref.size() == 0);
			log_assert(node.sort() == value.sort());
			mutate(node).append_arg(value._ref);
		} 
		Node input(IdString name, int width) {
			_ir.add_input(name, Sort(width));
			return add(NodeData(Fn::input, name), Sort(width), {});
		}
		Node state(IdString name, int width) {
			_ir.add_state(name, Sort(width));
			return add(NodeData(Fn::state, name), Sort(width), {});
		}
		Node state_memory(IdString name, int addr_width, int data_width) {
			_ir.add_state(name, Sort(addr_width, data_width));
			return add(NodeData(Fn::state, name), Sort(addr_width, data_width), {});
		}
		Node multiple(vector<Node> args, int width) {
			auto node = add(Fn::multiple, Sort(width), {});
			for(const auto &arg : args)
				mutate(node).append_arg(arg._ref);
			return node;
		}
		Node undriven(int width) {
			return add(Fn::undriven, Sort(width), {});
		}
		void declare_output(Node node, IdString name, int width) {
			_ir.add_output(name, Sort(width));
			mutate(node).assign_key({name, false});
		}
		void declare_state(Node node, IdString name, int width) {
			_ir.add_state(name, Sort(width));
			mutate(node).assign_key({name, true});
		}
		void declare_state_memory(Node node, IdString name, int addr_width, int data_width) {
			_ir.add_state(name, Sort(addr_width, data_width));
			mutate(node).assign_key({name, true});
		}
		void suggest_name(Node node, IdString name) {
			mutate(node).sparse_attr() = name;
		}
	};
	static FunctionalIR from_module(Module *module);
	Factory factory() { return Factory(*this); }
	int size() const { return _graph.size(); }
	Node operator[](int i) { return Node(_graph[i]); }
	void topological_sort();
	void forward_buf();
	dict<IdString, Sort> inputs() const { return _inputs; }
	dict<IdString, Sort> outputs() const { return _outputs; }
	dict<IdString, Sort> state() const { return _state; }
	Node get_output_node(IdString name) { return Node(_graph({name, false})); }
	Node get_state_next_node(IdString name) { return Node(_graph({name, true})); }
	class Iterator {
		friend class FunctionalIR;
		FunctionalIR *_ir;
		int _index;
		Iterator(FunctionalIR *ir, int index) : _ir(ir), _index(index) {}
	public:
		Node operator*() { return Node(_ir->_graph[_index]); }
		Iterator &operator++() { _index++; return *this; }
		bool operator!=(Iterator const &other) const { return _index != other._index; }
	};
	Iterator begin() { return Iterator(this, 0); }
	Iterator end() { return Iterator(this, _graph.size()); }
};

namespace FunctionalTools {
	template<class Id> class Scope {
	protected:
		char substitution_character = '_';
		virtual bool is_character_legal(char) = 0;
	private:
		pool<std::string> _used_names;
		dict<Id, std::string> _by_id;
	public:
		void reserve(std::string name) {
			_used_names.insert(std::move(name));
		}
		std::string unique_name(IdString suggestion) {
			std::string str = RTLIL::unescape_id(suggestion);
			for(size_t i = 0; i < str.size(); i++)
				if(!is_character_legal(str[i]))
					str[i] = substitution_character;
			if(_used_names.count(str) == 0) {
				_used_names.insert(str);
				return str;
			}
			for (int idx = 0 ; ; idx++){
				std::string suffixed = str + "_" + std::to_string(idx);
				if(_used_names.count(suffixed) == 0) {
					_used_names.insert(suffixed);
					return suffixed;
				}
			}
		}
		std::string operator()(Id id, IdString suggestion) {
			auto it = _by_id.find(id);
			if(it != _by_id.end())
				return it->second;
			std::string str = unique_name(suggestion);
			_by_id.insert({id, str});
			return str;
		}
	};
	class Writer {
		std::ostream *os;
		void print_impl(const char *fmt, vector<std::function<void()>>& fns);
	public:
		Writer(std::ostream &os) : os(&os) {}
		template<class T> Writer& operator <<(T&& arg) { *os << std::forward<T>(arg); return *this; }
		template<typename... Args>
		void print(const char *fmt, Args&&... args)
		{
			vector<std::function<void()>> fns { [&]() { *this << args; }... };
			print_impl(fmt, fns);
		}
		template<typename Fn, typename... Args>
		void print_with(Fn fn, const char *fmt, Args&&... args)
		{
			vector<std::function<void()>> fns { [&]() {
				if constexpr (std::is_invocable_v<Fn, Args>)
					*this << fn(args);
				else
					*this << args; }...
			};
			print_impl(fmt, fns);
		}
	};
}

YOSYS_NAMESPACE_END

#endif
