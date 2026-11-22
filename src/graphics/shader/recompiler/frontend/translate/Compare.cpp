#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <bit>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

void Translator::EmitCompareResult(const Decoder::Instruction& inst, IR::U1 value, bool scalar) {
	if (scalar) {
		ir.SetScc(value);
		return;
	}
	WriteVectorMask(inst.dst, value);
}

void Translator::EmitCompareConstant(const Decoder::Instruction& inst, bool value, bool scalar) {
	EmitCompareResult(inst, IR::U1(IR::Value(value)), scalar);
}

void Translator::EmitIntegerCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                    IR::Type type, bool scalar, bool swap_operands) {
	// The equality compares share their IR opcode with the unsigned ones.
	const bool signed_64 = opcode == IR::ValueOpcode::SLessThan64 ||
	                       opcode == IR::ValueOpcode::SLessThanEqual64 ||
	                       inst.opcode == Decoder::Opcode::V_CMP_EQ_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMPX_EQ_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMP_NE_I64 ||
	                       inst.opcode == Decoder::Opcode::V_CMPX_NE_I64;
	const auto read = [&](const Decoder::Operand& operand) {
		// RDNA2 expands signed 64-bit integer literals by sign extension.
		if (signed_64 && operand.kind == Decoder::OperandKind::LiteralConstant) {
			return IR::Value(static_cast<uint64_t>(
			    static_cast<int64_t>(std::bit_cast<int32_t>(operand.value))));
		}
		return ReadOperand(operand, type);
	};
	auto lhs = read(inst.src0);
	auto rhs = read(inst.src1);
	if (swap_operands) {
		std::swap(lhs, rhs);
	}
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), scalar);
}

void Translator::EmitInteger16Compare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                      bool signed_value) {
	const auto lhs = ReadU16AsU32(inst.src0, signed_value);
	const auto rhs = ReadU16AsU32(inst.src1, signed_value);
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs})), false);
}

void Translator::EmitFloatCompare(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                  bool half) {
	const auto type = IR::ArgTypeOf(opcode, 0);
	const auto lhs =
	    half ? IR::Value(ReadF16AsF32(inst.src0)) : ReadOperand(inst.src0, type);
	const auto rhs =
	    half ? IR::Value(ReadF16AsF32(inst.src1)) : ReadOperand(inst.src1, type);
	const IR::FPCompareFlags flags{
	    .flush_input_denorms = !half && type == IR::Type::F32 && flush_f32_inputs};
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {lhs, rhs}, flags)), false);
}

void Translator::EmitFloatOrderedCompare(const Decoder::Instruction& inst, bool ordered,
                                         IR::Type type) {
	const auto is_nan_op =
	    type == IR::Type::F64 ? IR::ValueOpcode::FPIsNan64 : IR::ValueOpcode::FPIsNan32;
	const auto is_nan = [&](const Decoder::Operand& operand) {
		const auto value =
		    type == IR::Type::F16 ? IR::Value(ReadF16AsF32(operand)) : ReadOperand(operand, type);
		return IR::U1(ir.Emit(is_nan_op, {value}));
	};
	const auto lhs       = is_nan(inst.src0);
	const auto unordered = ir.LogicalOr(lhs, is_nan(inst.src1));
	EmitCompareResult(inst, ordered ? ir.LogicalNot(unordered) : unordered, false);
}

void Translator::EmitFloatClassCompare(const Decoder::Instruction& inst, bool half) {
	const auto value = half ? IR::Value(Read16LaneBits(inst.src0, false))
	                        : ReadOperand(inst.src0, IR::Type::F32);
	const auto mask = half ? IR::Value(Read16LaneBits(inst.src1, false))
	                      : ReadOperand(inst.src1, IR::Type::U32);
	const auto opcode = half ? IR::ValueOpcode::FPCmpClass16 : IR::ValueOpcode::FPCmpClass32;
	EmitCompareResult(inst, IR::U1(ir.Emit(opcode, {value, mask})), false);
}

void Translator::EmitFloatClassCompare64(const Decoder::Instruction& inst) {
	// RDNA 2 V_CMP_CLASS_F64 / V_CMPX_CLASS_F64:
	//   bit 0: sNaN, bit 1: qNaN, bit 2: -Inf, bit 3: -Normal, bit 4: -Subnormal,
	//   bit 5: -0,   bit 6: +0,   bit 7: +Subnormal, bit 8: +Normal, bit 9: +Inf
	const auto value = ReadOperand(inst.src0, IR::Type::F64);
	const auto mask  = ReadU32(inst.src1);
	const auto bits  = ir.Emit(IR::ValueOpcode::BitCastU64F64, {value});
	const auto lo    = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {bits, IR::Value(0u)}));
	const auto hi    = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {bits, IR::Value(1u)}));

	const auto neg = ir.INotEqual(ir.BitwiseAnd(hi, IR::U32(IR::Value(0x80000000u))), IR::U32(IR::Value(0u)));
	const auto pos = ir.LogicalNot(neg);
	const auto exp = IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {hi, IR::Value(20u), IR::Value(11u)}));
	const auto mant_hi = ir.BitwiseAnd(hi, IR::U32(IR::Value(0x000fffffu)));
	const auto mant_zero =
	    ir.LogicalAnd(ir.IEqual(mant_hi, IR::U32(IR::Value(0u))), ir.IEqual(lo, IR::U32(IR::Value(0u))));
	const auto mant_nz = ir.LogicalNot(mant_zero);
	const auto qbit    = ir.INotEqual(ir.BitwiseAnd(hi, IR::U32(IR::Value(0x00080000u))), IR::U32(IR::Value(0u)));

	const auto exp_max  = ir.IEqual(exp, IR::U32(IR::Value(0x7ffu)));
	const auto exp_zero = ir.IEqual(exp, IR::U32(IR::Value(0u)));
	const auto exp_norm = ir.LogicalAnd(ir.LogicalNot(exp_max), ir.LogicalNot(exp_zero));

	const auto is_snan    = ir.LogicalAnd(exp_max, ir.LogicalAnd(mant_nz, ir.LogicalNot(qbit)));
	const auto is_qnan    = ir.LogicalAnd(exp_max, qbit);
	const auto is_inf     = ir.LogicalAnd(exp_max, mant_zero);
	const auto is_zero    = ir.LogicalAnd(exp_zero, mant_zero);
	const auto is_subnorm = ir.LogicalAnd(exp_zero, mant_nz);

	auto class_bits = IR::U32(IR::Value(0u));
	auto add_bit    = [&](IR::U1 cond, uint32_t bit) {
		class_bits = ir.BitwiseOr(class_bits,
		                          ir.Select(cond, IR::U32(IR::Value(1u << bit)), IR::U32(IR::Value(0u))));
	};
	add_bit(is_snan, 0u);
	add_bit(is_qnan, 1u);
	add_bit(ir.LogicalAnd(neg, is_inf), 2u);
	add_bit(ir.LogicalAnd(neg, exp_norm), 3u);
	add_bit(ir.LogicalAnd(neg, is_subnorm), 4u);
	add_bit(ir.LogicalAnd(neg, is_zero), 5u);
	add_bit(ir.LogicalAnd(pos, is_zero), 6u);
	add_bit(ir.LogicalAnd(pos, is_subnorm), 7u);
	add_bit(ir.LogicalAnd(pos, exp_norm), 8u);
	add_bit(ir.LogicalAnd(pos, is_inf), 9u);

	const auto matched = ir.INotEqual(ir.BitwiseAnd(class_bits, mask), IR::U32(IR::Value(0u)));
	EmitCompareResult(inst, matched, false);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
