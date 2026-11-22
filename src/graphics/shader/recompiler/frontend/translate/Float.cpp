#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>
#include <bit>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

void Translator::PackedFloat16(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                               bool accumulator, bool quiet_snan) {
	const auto translate_lane = [&](bool high) {
		const auto lhs = ReadF16LaneAsF32(inst.src0, high);
		const auto rhs = ReadF16LaneAsF32(inst.src1, high);
		IR::F32    result;
		if (accumulator) {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs, ReadF16LaneAsF32(inst.dst, high)}));
		} else if (inst.src_count == 3u) {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs, ReadF16LaneAsF32(inst.src2, high)}));
		} else {
			result = IR::F32(ir.Emit(opcode, {lhs, rhs}));
		}
		return ApplyF32ResultModifiers(inst.dst, result);
	};
	auto raw    = DestinationOperand(inst);
	raw.omod    = 0u;
	raw.clamp   = false;
	auto result = PackHalf2x16(translate_lane(false), translate_lane(true));
	if (quiet_snan) {
		const auto quiet_snan_lane = [&](const Decoder::Operand& operand, bool high) {
			const auto bits     = ReadU16LaneAsU32(operand, high, false);
			const auto exponent = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x7e00u)));
			const auto payload  = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x01ffu)));
			const auto snan     = ir.LogicalAnd(ir.IEqual(exponent, IR::U32(IR::Value(0x7c00u))),
			                                    ir.INotEqual(payload, IR::U32(IR::Value(0u))));
			return std::pair {snan, ir.BitwiseOr(bits, IR::U32(IR::Value(0x0200u)))};
		};
		const auto override_lane = [&](bool high) {
			const auto [lhs_snan, lhs_quiet] = quiet_snan_lane(inst.src0, high);
			const auto [rhs_snan, rhs_quiet] = quiet_snan_lane(inst.src1, high);
			const auto normal = high ? ir.ShiftRightLogical(result, IR::U32(IR::Value(16u)))
			                         : ir.BitwiseAnd(result, IR::U32(IR::Value(0xffffu)));
			return ir.Select(lhs_snan, lhs_quiet, ir.Select(rhs_snan, rhs_quiet, normal));
		};
		result = PackU16Lanes(override_lane(false), override_lane(true));
	}
	WriteOperand(raw, result);
}

void Translator::Float16Unary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                              bool invalid_negative) {
	const auto argument = ReadF16AsF32(inst.src0);
	auto       result   = IR::F32(ir.Emit(opcode, {argument}));
	if (!invalid_negative) {
		WriteF16(DestinationOperand(inst), result);
		return;
	}
	const auto negative =
	    IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {argument, IR::Value::F32(0.0f)}));
	result             = ApplyF32ResultModifiers(inst.dst, result);
	const auto bits    = PackHalf2x16(result, IR::F32(IR::Value::F32(0.0f)));
	const auto invalid = IR::U32(IR::Value(inst.dst.clamp ? 0u : 0xfe00u));
	Write16Bits(DestinationOperand(inst), ir.Select(negative, invalid, bits));
}

void Translator::Float16Trig(const Decoder::Instruction& inst, IR::ValueOpcode opcode) {
	const auto argument = ReadF16AsF32(inst.src0);
	const auto magnitude =
	    ir.BitwiseAnd(ir.BitCastU32(argument), IR::U32(IR::Value(0x7fffffffu)));
	auto result = IR::F32(ir.Emit(opcode, {argument}));
	if (opcode == IR::ValueOpcode::FPSin) {
		const auto fraction = IR::F32(ir.Emit(IR::ValueOpcode::FPFract32, {argument}));
		const auto whole = IR::U1(
		    ir.Emit(IR::ValueOpcode::FPOrdEqual32, {fraction, IR::Value::F32(0.0f)}));
		const auto half = IR::U1(
		    ir.Emit(IR::ValueOpcode::FPOrdEqual32, {fraction, IR::Value::F32(0.5f)}));
		const auto nonzero = ir.INotEqual(magnitude, IR::U32(IR::Value(0u)));
		const auto cardinal = ir.LogicalOr(ir.LogicalAnd(whole, nonzero), half);
		result = IR::F32(ir.Emit(
		    IR::ValueOpcode::SelectF32, {cardinal, IR::Value::F32(0.0f), result}));
	} else {
		// The architectural quarter-cycle result is exactly zero. Evaluating cos(pi/2) with a
		// rounded F32 PI can otherwise produce a half-precision subnormal instead.
		const auto fraction = IR::F32(ir.Emit(IR::ValueOpcode::FPFract32, {argument}));
		const auto quarter = IR::U1(
		    ir.Emit(IR::ValueOpcode::FPOrdEqual32, {fraction, IR::Value::F32(0.25f)}));
		const auto three_quarters = IR::U1(
		    ir.Emit(IR::ValueOpcode::FPOrdEqual32, {fraction, IR::Value::F32(0.75f)}));
		result = IR::F32(
		    ir.Emit(IR::ValueOpcode::SelectF32,
		            {ir.LogicalOr(quarter, three_quarters), IR::Value::F32(0.0f), result}));
	}

	const auto infinite = ir.IEqual(magnitude, IR::U32(IR::Value(0x7f800000u)));
	result              = ApplyF32ResultModifiers(inst.dst, result);
	const auto bits    = PackHalf2x16(result, IR::F32(IR::Value::F32(0.0f)));
	const auto invalid = IR::U32(IR::Value(inst.dst.clamp ? 0u : 0xfe00u));
	Write16Bits(DestinationOperand(inst), ir.Select(infinite, invalid, bits));
}

void Translator::Float16Binary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                               bool reverse) {
	const auto lhs = ReadF16AsF32(reverse ? inst.src1 : inst.src0);
	const auto rhs = ReadF16AsF32(reverse ? inst.src0 : inst.src1);
	WriteF16(DestinationOperand(inst), IR::F32(ir.Emit(opcode, {lhs, rhs})));
}

void Translator::Float16Ternary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                                bool accumulator, bool mix) {
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = accumulator && index == 2u ? inst.dst : SourceAt(inst, index);
		args[index] = mix ? IR::Value(ReadMixF32(operand)) : IR::Value(ReadF16AsF32(operand));
	}
	WriteF16(DestinationOperand(inst), IR::F32(ir.Emit(opcode, {args[0], args[1], args[2]})));
}

void Translator::FloatUnary(const Decoder::Instruction& inst, IR::ValueOpcode opcode) {
	const auto type = IR::ArgTypeOf(opcode, 0);
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {ReadOperand(inst.src0, type)}));
}

void Translator::V_FRACT(const Decoder::Instruction& inst, IR::Type type) {
	// x - floor(x) rounds to 1.0 for a tiny negative x; v_fract returns the largest value below
	// one instead. The ordered compare lets NaN through.
	const auto destination = DestinationOperand(inst);
	if (type == IR::Type::F64) {
		const auto result    = ir.Emit(IR::ValueOpcode::FPFract64, {ReadOperand(inst.src0, type)});
		const auto below_one = ir.Emit(IR::ValueOpcode::BitCastF64U64,
		                               {IR::Value(uint64_t {0x3fefffffffffffff})});
		const auto above     = ir.Emit(IR::ValueOpcode::FPOrdGreaterThan64, {result, below_one});
		WriteOperand(destination, ir.Emit(IR::ValueOpcode::SelectF64, {above, below_one, result}));
		return;
	}
	const bool half   = type == IR::Type::F16;
	const auto source = half ? ReadF16AsF32(inst.src0) : IR::F32(ReadOperand(inst.src0, type));
	const auto result = IR::F32(ir.Emit(IR::ValueOpcode::FPFract32, {source}));
	// Clamp in the destination precision: 0x3f7fe000 is the half 0x3bff.
	const auto below_one =
	    IR::F32(IR::Value::F32(std::bit_cast<float>(half ? 0x3f7fe000u : 0x3f7fffffu)));
	const auto above   = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThan32, {result, below_one}));
	const auto clamped = SelectF32(above, below_one, result);
	if (half) {
		WriteF16(destination, clamped);
	} else {
		WriteOperand(destination, clamped);
	}
}

void Translator::FloatBinary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                             bool reverse) {
	std::array<IR::Value, 2> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = SourceAt(inst, reverse ? 1u - index : index);
		args[index]        = ReadOperand(operand, IR::ArgTypeOf(opcode, index));
	}
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {args[0], args[1]}));
}

void Translator::FloatTernary(const Decoder::Instruction& inst, IR::ValueOpcode opcode,
                              bool accumulator, bool mix) {
	std::array<IR::Value, 3> args;
	for (uint32_t index = 0; index < args.size(); index++) {
		const auto& operand = accumulator && index == 2u ? inst.dst : SourceAt(inst, index);
		const auto type    = IR::ArgTypeOf(opcode, index);
		args[index]        = type == IR::Type::F32 && mix ? IR::Value(ReadMixF32(operand))
		                                                  : ReadOperand(operand, type);
	}
	WriteOperand(DestinationOperand(inst), ir.Emit(opcode, {args[0], args[1], args[2]}));
}

void Translator::V_DIV_SCALE(const Decoder::Instruction& inst, bool is64) {
	// RDNA 2 IEEE-754 division pre-scaling:
	//   src0 = operand to scale (either denom or numer)
	//   src1 = denom
	//   src2 = numer
	//   dst  = scaled src0
	//   dst2 = per-lane scale flag (written to sdst, typically VCC, consumed by V_DIV_FMAS)
	const bool write_sdst = inst.dst2.kind != Decoder::OperandKind::Null &&
	                        inst.dst2.kind != Decoder::OperandKind::Unknown;

	if (!is64) {
		const auto s0    = IR::F32(ReadOperand(inst.src0, IR::Type::F32));
		const auto denom = IR::F32(ReadOperand(inst.src1, IR::Type::F32));
		const auto numer = IR::F32(ReadOperand(inst.src2, IR::Type::F32));

		const auto s0_bits    = ir.BitCastU32(s0);
		const auto denom_bits = ir.BitCastU32(denom);
		const auto numer_bits = ir.BitCastU32(numer);

		const auto denom_abs = ir.BitwiseAnd(denom_bits, IR::U32(IR::Value(0x7fffffffu)));
		const auto numer_abs = ir.BitwiseAnd(numer_bits, IR::U32(IR::Value(0x7fffffffu)));

		const auto exp_d =
		    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {denom_bits, IR::Value(23u), IR::Value(8u)}));
		const auto exp_n =
		    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {numer_bits, IR::Value(23u), IR::Value(8u)}));

		const auto special_d = ir.LogicalOr(ir.IEqual(denom_abs, IR::U32(IR::Value(0u))),
		                                    ir.IEqual(exp_d, IR::U32(IR::Value(0xffu))));
		const auto special_n = ir.LogicalOr(ir.IEqual(numer_abs, IR::U32(IR::Value(0u))),
		                                    ir.IEqual(exp_n, IR::U32(IR::Value(0xffu))));
		const auto is_special = ir.LogicalOr(special_d, special_n);

		const auto s0_is_numer = ir.IEqual(s0_bits, numer_bits);
		const auto s0_is_denom = ir.IEqual(s0_bits, denom_bits);

		// Scale up by 2^32 when quotient would underflow (exp_d - exp_n >= 96) or operand is tiny.
		const auto exp_n_plus_96 = ir.IAdd(exp_n, IR::U32(IR::Value(96u)));
		const auto ratio_tiny =
		    IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThanEqual32, {exp_d, exp_n_plus_96}));
		const auto denom_tiny =
		    IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {exp_d, IR::U32(IR::Value(31u))}));
		const auto numer_tiny =
		    IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {exp_n, IR::U32(IR::Value(31u))}));
		const auto denom_huge =
		    IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThan32, {exp_d, IR::U32(IR::Value(231u))}));

		const auto scale_up_2p32 =
		    IR::F32(ir.Emit(IR::ValueOpcode::FPMul32,
		                    {s0, IR::Value::F32(std::bit_cast<float>(0x4f800000u))})); // * 2^32
		const auto scale_dn_2m32 =
		    IR::F32(ir.Emit(IR::ValueOpcode::FPMul32,
		                    {s0, IR::Value::F32(std::bit_cast<float>(0x2f800000u))})); // * 2^-32

		const auto need_scale_up = ir.LogicalOr(
		    denom_tiny, ir.LogicalAnd(ir.LogicalOr(ratio_tiny, numer_tiny), s0_is_numer));
		const auto need_scale_dn = ir.LogicalAnd(
		    ir.LogicalNot(need_scale_up), ir.LogicalAnd(denom_huge, s0_is_denom));

		const auto scaled = SelectF32(need_scale_up, scale_up_2p32,
		                              SelectF32(need_scale_dn, scale_dn_2m32, s0));
		const auto result = SelectF32(is_special, s0, scaled);

		const auto flag_raw = ir.LogicalOr(ir.LogicalOr(ratio_tiny, numer_tiny), denom_huge);
		const auto flag =
		    IR::U1(ir.Emit(IR::ValueOpcode::SelectU1, {is_special, IR::Value(false), flag_raw}));

		WriteOperand(DestinationOperand(inst), result);
		if (write_sdst) {
			WriteVectorMask(inst.dst2, flag);
		}
		return;
	}

	// F64 path
	const auto s0    = ReadOperand(inst.src0, IR::Type::F64);
	const auto denom = ReadOperand(inst.src1, IR::Type::F64);
	const auto numer = ReadOperand(inst.src2, IR::Type::F64);

	const auto s0_bits    = ir.Emit(IR::ValueOpcode::BitCastU64F64, {s0});
	const auto denom_bits = ir.Emit(IR::ValueOpcode::BitCastU64F64, {denom});
	const auto numer_bits = ir.Emit(IR::ValueOpcode::BitCastU64F64, {numer});

	const auto s0_hi    = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {s0_bits, IR::Value(1u)}));
	const auto denom_lo = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {denom_bits, IR::Value(0u)}));
	const auto denom_hi = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {denom_bits, IR::Value(1u)}));
	const auto numer_lo = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {numer_bits, IR::Value(0u)}));
	const auto numer_hi = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {numer_bits, IR::Value(1u)}));

	const auto denom_abs_hi = ir.BitwiseAnd(denom_hi, IR::U32(IR::Value(0x7fffffffu)));
	const auto numer_abs_hi = ir.BitwiseAnd(numer_hi, IR::U32(IR::Value(0x7fffffffu)));

	const auto exp_d =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {denom_hi, IR::Value(20u), IR::Value(11u)}));
	const auto exp_n =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {numer_hi, IR::Value(20u), IR::Value(11u)}));

	const auto denom_zero = ir.LogicalAnd(ir.IEqual(denom_abs_hi, IR::U32(IR::Value(0u))),
	                                      ir.IEqual(denom_lo, IR::U32(IR::Value(0u))));
	const auto numer_zero = ir.LogicalAnd(ir.IEqual(numer_abs_hi, IR::U32(IR::Value(0u))),
	                                      ir.IEqual(numer_lo, IR::U32(IR::Value(0u))));
	const auto is_special = ir.LogicalOr(
	    ir.LogicalOr(denom_zero, ir.IEqual(exp_d, IR::U32(IR::Value(0x7ffu)))),
	    ir.LogicalOr(numer_zero, ir.IEqual(exp_n, IR::U32(IR::Value(0x7ffu)))));

	const auto s0_is_numer = IR::U1(ir.Emit(IR::ValueOpcode::IEqual64, {s0_bits, numer_bits}));
	const auto s0_is_denom = IR::U1(ir.Emit(IR::ValueOpcode::IEqual64, {s0_bits, denom_bits}));

	const auto exp_n_plus_768 = ir.IAdd(exp_n, IR::U32(IR::Value(768u)));
	const auto ratio_tiny =
	    IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThanEqual32, {exp_d, exp_n_plus_768}));
	const auto denom_tiny =
	    IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {exp_d, IR::U32(IR::Value(64u))}));
	const auto numer_tiny =
	    IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {exp_n, IR::U32(IR::Value(64u))}));
	const auto denom_huge =
	    IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThan32, {exp_d, IR::U32(IR::Value(1983u))}));

	const auto scale_2p64 =
	    ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x43f0000000000000ull})});
	const auto scale_2m64 =
	    ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x3bf0000000000000ull})});
	const auto scaled_up = ir.Emit(IR::ValueOpcode::FPMul64, {s0, scale_2p64});
	const auto scaled_dn = ir.Emit(IR::ValueOpcode::FPMul64, {s0, scale_2m64});

	const auto need_scale_up = ir.LogicalOr(
	    denom_tiny, ir.LogicalAnd(ir.LogicalOr(ratio_tiny, numer_tiny), s0_is_numer));
	const auto need_scale_dn =
	    ir.LogicalAnd(ir.LogicalNot(need_scale_up), ir.LogicalAnd(denom_huge, s0_is_denom));

	const auto scaled = ir.Emit(
	    IR::ValueOpcode::SelectF64,
	    {need_scale_up, scaled_up, ir.Emit(IR::ValueOpcode::SelectF64, {need_scale_dn, scaled_dn, s0})});
	const auto result = ir.Emit(IR::ValueOpcode::SelectF64, {is_special, s0, scaled});

	const auto flag_raw = ir.LogicalOr(ir.LogicalOr(ratio_tiny, numer_tiny), denom_huge);
	const auto flag =
	    IR::U1(ir.Emit(IR::ValueOpcode::SelectU1, {is_special, IR::Value(false), flag_raw}));
	(void)s0_hi;

	WriteOperand(DestinationOperand(inst), result);
	if (write_sdst) {
		WriteVectorMask(inst.dst2, flag);
	}
}

void Translator::V_DIV_FMAS(const Decoder::Instruction& inst, bool is64) {
	// RDNA 2 scaled FMA for IEEE-754 division:
	//   res = fma(src0, src1, src2)
	//   if (VCC) res *= (abs(src2) >= 2.0 ? 2^N : 2^-N)  (N = 32 for F32, 64 for F64)
	const auto vcc = ir.GetVcc();
	if (!is64) {
		const auto s0  = IR::F32(ReadOperand(inst.src0, IR::Type::F32));
		const auto s1  = IR::F32(ReadOperand(inst.src1, IR::Type::F32));
		const auto s2  = IR::F32(ReadOperand(inst.src2, IR::Type::F32));
		const auto fma = IR::F32(ir.Emit(IR::ValueOpcode::FPFma32, {s0, s1, s2}));
		const auto s2_abs = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {s2}));
		const auto large =
		    IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {s2_abs, IR::Value::F32(2.0f)}));
		const auto scale = SelectF32(large, IR::F32(IR::Value::F32(std::bit_cast<float>(0x4f800000u))),
		                             IR::F32(IR::Value::F32(std::bit_cast<float>(0x2f800000u))));
		const auto scaled = IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {fma, scale}));
		WriteOperand(DestinationOperand(inst), SelectF32(vcc, scaled, fma));
		return;
	}

	const auto s0  = ReadOperand(inst.src0, IR::Type::F64);
	const auto s1  = ReadOperand(inst.src1, IR::Type::F64);
	const auto s2  = ReadOperand(inst.src2, IR::Type::F64);
	const auto fma = ir.Emit(IR::ValueOpcode::FPFma64, {s0, s1, s2});

	const auto s2_bits = ir.Emit(IR::ValueOpcode::BitCastU64F64, {s2});
	const auto s2_lo   = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {s2_bits, IR::Value(0u)}));
	const auto s2_hi   = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {s2_bits, IR::Value(1u)}));
	const auto s2_abs  = ir.Emit(
        IR::ValueOpcode::BitCastF64U64,
        {ir.Emit(IR::ValueOpcode::CompositeConstructU64,
                 {s2_lo, ir.BitwiseAnd(s2_hi, IR::U32(IR::Value(0x7fffffffu)))})});
	const auto two_f64 =
	    ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x4000000000000000ull})});
	const auto large = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual64, {s2_abs, two_f64}));
	const auto scale_2p64 =
	    ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x43f0000000000000ull})});
	const auto scale_2m64 =
	    ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x3bf0000000000000ull})});
	const auto scale  = ir.Emit(IR::ValueOpcode::SelectF64, {large, scale_2p64, scale_2m64});
	const auto scaled = ir.Emit(IR::ValueOpcode::FPMul64, {fma, scale});
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::SelectF64, {vcc, scaled, fma}));
}

void Translator::V_DIV_FIXUP(const Decoder::Instruction& inst, bool is64) {
	// RDNA 2 IEEE-754 division special-case fixup:
	//   src0 = quotient approximation (q)
	//   src1 = denominator (d)
	//   src2 = numerator (n)
	if (!is64) {
		const auto q     = IR::F32(ReadOperand(inst.src0, IR::Type::F32));
		const auto denom = IR::F32(ReadOperand(inst.src1, IR::Type::F32));
		const auto numer = IR::F32(ReadOperand(inst.src2, IR::Type::F32));

		const auto q_bits     = ir.BitCastU32(q);
		const auto denom_bits = ir.BitCastU32(denom);
		const auto numer_bits = ir.BitCastU32(numer);

		const auto sign_out =
		    ir.BitwiseAnd(ir.BitwiseXor(denom_bits, numer_bits), IR::U32(IR::Value(0x80000000u)));
		const auto denom_abs = ir.BitwiseAnd(denom_bits, IR::U32(IR::Value(0x7fffffffu)));
		const auto numer_abs = ir.BitwiseAnd(numer_bits, IR::U32(IR::Value(0x7fffffffu)));

		const auto q_nan     = IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {q}));
		const auto denom_nan = IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {denom}));
		const auto numer_nan = IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan32, {numer}));
		const auto denom_inf = ir.IEqual(denom_abs, IR::U32(IR::Value(0x7f800000u)));
		const auto numer_inf = ir.IEqual(numer_abs, IR::U32(IR::Value(0x7f800000u)));
		const auto denom_z   = ir.IEqual(denom_abs, IR::U32(IR::Value(0u)));
		const auto numer_z   = ir.IEqual(numer_abs, IR::U32(IR::Value(0u)));

		const auto q_signed =
		    ir.BitCastF32(ir.BitwiseOr(ir.BitwiseAnd(q_bits, IR::U32(IR::Value(0x7fffffffu))), sign_out));
		const auto signed_inf  = ir.BitCastF32(ir.BitwiseOr(sign_out, IR::U32(IR::Value(0x7f800000u))));
		const auto signed_zero = ir.BitCastF32(sign_out);
		const auto qnan        = IR::F32(IR::Value::F32(std::bit_cast<float>(0x7fc00000u)));

		const auto indet =
		    ir.LogicalOr(ir.LogicalAnd(numer_inf, denom_inf), ir.LogicalAnd(numer_z, denom_z));
		const auto ret_inf  = ir.LogicalOr(numer_inf, denom_z);
		const auto ret_zero = ir.LogicalOr(denom_inf, numer_z);

		auto res = SelectF32(q_nan, signed_inf, q_signed);
		res      = SelectF32(ret_zero, signed_zero, res);
		res      = SelectF32(ret_inf, signed_inf, res);
		res      = SelectF32(indet, qnan, res);
		res      = SelectF32(denom_nan, qnan, res);
		res      = SelectF32(numer_nan, qnan, res);

		WriteOperand(DestinationOperand(inst), res);
		return;
	}

	const auto q     = ReadOperand(inst.src0, IR::Type::F64);
	const auto denom = ReadOperand(inst.src1, IR::Type::F64);
	const auto numer = ReadOperand(inst.src2, IR::Type::F64);

	const auto q_bits     = ir.Emit(IR::ValueOpcode::BitCastU64F64, {q});
	const auto denom_bits = ir.Emit(IR::ValueOpcode::BitCastU64F64, {denom});
	const auto numer_bits = ir.Emit(IR::ValueOpcode::BitCastU64F64, {numer});

	const auto q_lo     = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {q_bits, IR::Value(0u)}));
	const auto q_hi     = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {q_bits, IR::Value(1u)}));
	const auto denom_lo = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {denom_bits, IR::Value(0u)}));
	const auto denom_hi = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {denom_bits, IR::Value(1u)}));
	const auto numer_lo = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {numer_bits, IR::Value(0u)}));
	const auto numer_hi = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {numer_bits, IR::Value(1u)}));

	const auto sign_out =
	    ir.BitwiseAnd(ir.BitwiseXor(denom_hi, numer_hi), IR::U32(IR::Value(0x80000000u)));
	const auto denom_abs_hi = ir.BitwiseAnd(denom_hi, IR::U32(IR::Value(0x7fffffffu)));
	const auto numer_abs_hi = ir.BitwiseAnd(numer_hi, IR::U32(IR::Value(0x7fffffffu)));

	const auto q_nan     = IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan64, {q}));
	const auto denom_nan = IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan64, {denom}));
	const auto numer_nan = IR::U1(ir.Emit(IR::ValueOpcode::FPIsNan64, {numer}));
	const auto denom_inf = ir.LogicalAnd(ir.IEqual(denom_abs_hi, IR::U32(IR::Value(0x7ff00000u))),
	                                     ir.IEqual(denom_lo, IR::U32(IR::Value(0u))));
	const auto numer_inf = ir.LogicalAnd(ir.IEqual(numer_abs_hi, IR::U32(IR::Value(0x7ff00000u))),
	                                     ir.IEqual(numer_lo, IR::U32(IR::Value(0u))));
	const auto denom_z   = ir.LogicalAnd(ir.IEqual(denom_abs_hi, IR::U32(IR::Value(0u))),
	                                     ir.IEqual(denom_lo, IR::U32(IR::Value(0u))));
	const auto numer_z   = ir.LogicalAnd(ir.IEqual(numer_abs_hi, IR::U32(IR::Value(0u))),
	                                     ir.IEqual(numer_lo, IR::U32(IR::Value(0u))));

	const auto q_signed = ir.Emit(
	    IR::ValueOpcode::BitCastF64U64,
	    {ir.Emit(IR::ValueOpcode::CompositeConstructU64,
	             {q_lo, ir.BitwiseOr(ir.BitwiseAnd(q_hi, IR::U32(IR::Value(0x7fffffffu))), sign_out)})});
	const auto signed_inf = ir.Emit(
	    IR::ValueOpcode::BitCastF64U64,
	    {ir.Emit(IR::ValueOpcode::CompositeConstructU64,
	             {IR::U32(IR::Value(0u)), ir.BitwiseOr(sign_out, IR::U32(IR::Value(0x7ff00000u)))})});
	const auto signed_zero = ir.Emit(
	    IR::ValueOpcode::BitCastF64U64,
	    {ir.Emit(IR::ValueOpcode::CompositeConstructU64, {IR::U32(IR::Value(0u)), sign_out})});
	const auto qnan =
	    ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x7ff8000000000000ull})});

	const auto indet =
	    ir.LogicalOr(ir.LogicalAnd(numer_inf, denom_inf), ir.LogicalAnd(numer_z, denom_z));
	const auto ret_inf  = ir.LogicalOr(numer_inf, denom_z);
	const auto ret_zero = ir.LogicalOr(denom_inf, numer_z);

	auto res = ir.Emit(IR::ValueOpcode::SelectF64, {q_nan, signed_inf, q_signed});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {ret_zero, signed_zero, res});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {ret_inf, signed_inf, res});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {indet, qnan, res});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {denom_nan, qnan, res});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {numer_nan, qnan, res});

	WriteOperand(DestinationOperand(inst), res);
}

void Translator::V_TRIG_PREOP_F64(const Decoder::Instruction& inst) {
	// RDNA 2 Payne-Hanek trig range reduction helper:
	//   src0 = F64 input (unused by table lookup, kept for register dependency),
	//   src1 = U32 chunk index (0..31) selecting 53-bit slices of 2/pi.
	(void)ReadOperand(inst.src0, IR::Type::F64);
	const auto idx = ir.BitwiseAnd(ReadU32(inst.src1), IR::U32(IR::Value(0x1fu)));

	// First 4 exact 53-bit IEEE-754 F64 chunks of 2/pi:
	//   chunk 0: 0x3fe45f306dc9c883 (2/pi ~ 0.63661977236758138)
	//   chunk 1: 0xbc86b01ec5417056 (-3.93573533503649717e-17)
	//   chunk 2: 0x3926447e493ad4ce ( 2.52237218516195729e-33)
	//   chunk 3: 0x35c9c882a53f84ea ( 1.65319437059417946e-49)
	const auto c0 = ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x3fe45f306dc9c883ull})});
	const auto c1 = ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0xbc86b01ec5417056ull})});
	const auto c2 = ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x3926447e493ad4ceull})});
	const auto c3 = ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0x35c9c882a53f84eaull})});
	const auto zero = ir.Emit(IR::ValueOpcode::BitCastF64U64, {IR::Value(uint64_t {0ull})});

	auto res = ir.Emit(IR::ValueOpcode::SelectF64, {ir.IEqual(idx, IR::U32(IR::Value(3u))), c3, zero});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {ir.IEqual(idx, IR::U32(IR::Value(2u))), c2, res});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {ir.IEqual(idx, IR::U32(IR::Value(1u))), c1, res});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {ir.IEqual(idx, IR::U32(IR::Value(0u))), c0, res});
	WriteOperand(DestinationOperand(inst), res);
}

void Translator::V_FREXP_MANT_F32(const Decoder::Instruction& inst) {
	const auto bits = ReadU32(inst.src0);
	const auto exponent =
	    IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {bits, IR::Value(23u), IR::Value(8u)}));
	const auto mantissa = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x007fffffu)));
	const auto sign     = ir.BitwiseAnd(bits, IR::U32(IR::Value(0x80000000u)));
	const auto base     = ir.BitwiseOr(sign, IR::U32(IR::Value(0x3f000000u)));
	const auto normal   = ir.BitwiseOr(base, mantissa);
	const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {mantissa}));
	const auto shift    = ir.ISub(IR::U32(IR::Value(23u)), msb);
	const auto fraction =
	    ir.BitwiseAnd(ir.ShiftLeftLogical(mantissa, shift), IR::U32(IR::Value(0x007fffffu)));
	const auto subnormal = ir.BitwiseOr(base, fraction);
	const auto zero      = ir.IEqual(mantissa, IR::U32(IR::Value(0u)));
	const auto finite    = ir.Select(ir.INotEqual(exponent, IR::U32(IR::Value(0u))), normal,
	                                 ir.Select(zero, bits, subnormal));
	const auto result    = ir.Select(ir.IEqual(exponent, IR::U32(IR::Value(0xffu))), bits, finite);
	WriteOperand(DestinationOperand(inst), ir.BitCastF32(result));
}

void Translator::V_FREXP_MANT_F64(const Decoder::Instruction& inst) {
	const auto source   = ReadOperand(inst.src0, IR::Type::F64);
	const auto bits     = ir.Emit(IR::ValueOpcode::BitCastU64F64, {source});
	const auto lo       = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {bits, IR::Value(0u)}));
	const auto hi       = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {bits, IR::Value(1u)}));
	const auto exponent = IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {hi, IR::Value(20u), IR::Value(11u)}));
	const auto mant_hi  = ir.BitwiseAnd(hi, IR::U32(IR::Value(0x000fffffu)));
	const auto sign     = ir.BitwiseAnd(hi, IR::U32(IR::Value(0x80000000u)));
	const auto base_hi  = ir.BitwiseOr(sign, IR::U32(IR::Value(0x3fe00000u)));

	const auto normal_f64 = ir.Emit(
	    IR::ValueOpcode::BitCastF64U64,
	    {ir.Emit(IR::ValueOpcode::CompositeConstructU64, {lo, ir.BitwiseOr(base_hi, mant_hi)})});

	const auto hi_nz   = ir.INotEqual(mant_hi, IR::U32(IR::Value(0u)));
	const auto lo_nz   = ir.INotEqual(lo, IR::U32(IR::Value(0u)));
	const auto is_zero = ir.LogicalNot(ir.LogicalOr(hi_nz, lo_nz));
	const auto msb_hi  = ir.IAdd(IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {mant_hi})), IR::U32(IR::Value(32u)));
	const auto msb_lo  = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {lo}));
	const auto msb     = ir.Select(hi_nz, msb_hi, msb_lo);
	const auto shift   = ir.ISub(IR::U32(IR::Value(52u)), msb);
	const auto shifted = ir.Emit(IR::ValueOpcode::ShiftLeftLogical64, {bits, shift});
	const auto sh_lo   = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {shifted, IR::Value(0u)}));
	const auto sh_hi   = ir.BitwiseAnd(
	    IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {shifted, IR::Value(1u)})),
	    IR::U32(IR::Value(0x000fffffu)));
	const auto subnorm_f64 = ir.Emit(
	    IR::ValueOpcode::BitCastF64U64,
	    {ir.Emit(IR::ValueOpcode::CompositeConstructU64, {sh_lo, ir.BitwiseOr(base_hi, sh_hi)})});

	const auto denorm_or_zero = ir.Emit(IR::ValueOpcode::SelectF64, {is_zero, source, subnorm_f64});
	const auto finite = ir.Emit(
	    IR::ValueOpcode::SelectF64,
	    {ir.INotEqual(exponent, IR::U32(IR::Value(0u))), normal_f64, denorm_or_zero});
	const auto result = ir.Emit(
	    IR::ValueOpcode::SelectF64,
	    {ir.IEqual(exponent, IR::U32(IR::Value(0x7ffu))), source, finite});
	WriteOperand(DestinationOperand(inst), result);
}

void Translator::V_LDEXP_F64(const Decoder::Instruction& inst) {
	const auto source = ReadOperand(inst.src0, IR::Type::F64);
	const auto delta  = ReadU32(inst.src1);
	const auto bits   = ir.Emit(IR::ValueOpcode::BitCastU64F64, {source});
	const auto lo     = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {bits, IR::Value(0u)}));
	const auto hi     = IR::U32(ir.Emit(IR::ValueOpcode::CompositeExtractU64, {bits, IR::Value(1u)}));

	const auto sign     = ir.BitwiseAnd(hi, IR::U32(IR::Value(0x80000000u)));
	const auto exponent = IR::U32(ir.Emit(IR::ValueOpcode::BitFieldUExtract, {hi, IR::Value(20u), IR::Value(11u)}));
	const auto mant_hi  = ir.BitwiseAnd(hi, IR::U32(IR::Value(0x000fffffu)));
	const auto is_zero  = ir.LogicalAnd(
	    ir.IEqual(exponent, IR::U32(IR::Value(0u))),
	    ir.LogicalAnd(ir.IEqual(mant_hi, IR::U32(IR::Value(0u))), ir.IEqual(lo, IR::U32(IR::Value(0u)))));
	const auto is_special = ir.LogicalOr(is_zero, ir.IEqual(exponent, IR::U32(IR::Value(0x7ffu))));

	const auto new_exp = ir.IAdd(exponent, delta);
	const auto ovf =
	    IR::U1(ir.Emit(IR::ValueOpcode::SGreaterThanEqual32, {new_exp, IR::U32(IR::Value(0x7ffu))}));
	const auto udf =
	    IR::U1(ir.Emit(IR::ValueOpcode::SLessThanEqual32, {new_exp, IR::U32(IR::Value(0u))}));

	const auto shifted_exp = ir.ShiftLeftLogical(ir.BitwiseAnd(new_exp, IR::U32(IR::Value(0x7ffu))),
	                                             IR::U32(IR::Value(20u)));
	const auto normal_f64  = ir.Emit(
        IR::ValueOpcode::BitCastF64U64,
        {ir.Emit(IR::ValueOpcode::CompositeConstructU64,
                 {lo, ir.BitwiseOr(ir.BitwiseOr(sign, shifted_exp), mant_hi)})});
	const auto inf_f64 = ir.Emit(
	    IR::ValueOpcode::BitCastF64U64,
	    {ir.Emit(IR::ValueOpcode::CompositeConstructU64,
	             {IR::U32(IR::Value(0u)), ir.BitwiseOr(sign, IR::U32(IR::Value(0x7ff00000u)))})});
	const auto zero_f64 = ir.Emit(
	    IR::ValueOpcode::BitCastF64U64,
	    {ir.Emit(IR::ValueOpcode::CompositeConstructU64, {IR::U32(IR::Value(0u)), sign})});

	auto res = ir.Emit(IR::ValueOpcode::SelectF64, {udf, zero_f64, normal_f64});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {ovf, inf_f64, res});
	res      = ir.Emit(IR::ValueOpcode::SelectF64, {is_special, source, res});
	WriteOperand(DestinationOperand(inst), res);
}

void Translator::V_DOT2C_F32_F16(const Decoder::Instruction& inst) {
	auto a          = inst.src0;
	a.op_sel        = false;
	a.op_sel_hi     = true;
	auto b          = inst.src1;
	b.op_sel        = false;
	b.op_sel_hi     = true;
	const auto a_lo = ReadF16LaneAsF32(a, false);
	const auto a_hi = ReadF16LaneAsF32(a, true);
	const auto b_lo = ReadF16LaneAsF32(b, false);
	const auto b_hi = ReadF16LaneAsF32(b, true);
	const auto acc  = ReadMixF32(inst.dst);
	const auto lo   = IR::F32(ir.Emit(IR::ValueOpcode::FPFma32, {a_lo, b_lo, acc}));
	WriteOperand(DestinationOperand(inst), ir.Emit(IR::ValueOpcode::FPFma32, {a_hi, b_hi, lo}));
}

void Translator::V_CUBEID_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 0u);
}

void Translator::V_CUBESC_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 1u);
}

void Translator::V_CUBETC_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 2u);
}

void Translator::V_CUBEMA_F32(const Decoder::Instruction& inst) {
	return FloatCube(inst, 3u);
}

void Translator::FloatCube(const Decoder::Instruction& inst, uint32_t result_kind) {
	const auto x  = ReadMixF32(inst.src0);
	const auto y  = ReadMixF32(inst.src1);
	const auto z  = ReadMixF32(inst.src2);
	const auto nx = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {x}));
	const auto ny = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {y}));
	const auto nz = IR::F32(ir.Emit(IR::ValueOpcode::FPNeg32, {z}));
	const auto ax = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {x}));
	const auto ay = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {y}));
	const auto az = IR::F32(ir.Emit(IR::ValueOpcode::FPAbs32, {z}));
	const auto z_face =
	    ir.LogicalAnd(IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {az, ax})),
	                  IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {az, ay})));
	const auto y_face = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {ay, ax}));
	const auto x_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {x, IR::Value::F32(0.0f)}));
	const auto y_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {y, IR::Value::F32(0.0f)}));
	const auto z_neg = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {z, IR::Value::F32(0.0f)}));
	const auto select_face = [&](IR::F32 x_value, IR::F32 y_value, IR::F32 z_value) {
		return SelectF32(z_face, z_value, SelectF32(y_face, y_value, x_value));
	};
	IR::F32 result;
	switch (result_kind) {
		case 0u:
			result = select_face(
			    SelectF32(x_neg, IR::F32(IR::Value::F32(1.0f)), IR::F32(IR::Value::F32(0.0f))),
			    SelectF32(y_neg, IR::F32(IR::Value::F32(3.0f)), IR::F32(IR::Value::F32(2.0f))),
			    SelectF32(z_neg, IR::F32(IR::Value::F32(5.0f)), IR::F32(IR::Value::F32(4.0f))));
			break;
		case 1u: result = select_face(SelectF32(x_neg, z, nz), x, SelectF32(z_neg, nx, x)); break;
		case 2u: result = select_face(ny, SelectF32(y_neg, nz, z), ny); break;
		case 3u:
			result = IR::F32(
			    ir.Emit(IR::ValueOpcode::FPMul32, {select_face(x, y, z), IR::Value::F32(2.0f)}));
			break;
		default: EXIT("invalid cube result kind");
	}
	WriteOperand(DestinationOperand(inst), result);
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
