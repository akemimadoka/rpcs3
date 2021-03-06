#pragma once

#ifdef LLVM_AVAILABLE

#include <unordered_map>
#include <map>
#include <unordered_set>
#include <set>
#include <array>

#include "../rpcs3/Emu/Cell/PPUOpcodes.h"
#include "../rpcs3/Emu/Cell/PPUAnalyser.h"

#include "restore_new.h"
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "define_new_memleakdetect.h"

#include "../Utilities/types.h"
#include "../Utilities/StrFmt.h"
#include "../Utilities/BEType.h"

template<typename T, typename = void>
struct TypeGen
{
	static_assert(!sizeof(T), "GetType<>() error: unknown type");
};

template<typename T>
struct TypeGen<T, std::enable_if_t<std::is_void<T>::value>>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getVoidTy(context); }
};

template<typename T>
struct TypeGen<T, std::enable_if_t<std::is_same<T, s64>::value || std::is_same<T, u64>::value>>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getInt64Ty(context); }
};

template<typename T>
struct TypeGen<T, std::enable_if_t<std::is_same<T, s32>::value || std::is_same<T, u32>::value>>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getInt32Ty(context); }
};

template<typename T>
struct TypeGen<T, std::enable_if_t<std::is_same<T, s16>::value || std::is_same<T, u16>::value>>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getInt16Ty(context); }
};

template<typename T>
struct TypeGen<T, std::enable_if_t<std::is_same<T, s8>::value || std::is_same<T, u8>::value || std::is_same<T, char>::value>>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getInt8Ty(context); }
};

template<>
struct TypeGen<f32, void>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getFloatTy(context); }
};

template<>
struct TypeGen<f64, void>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getDoubleTy(context); }
};

template<>
struct TypeGen<bool, void>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getInt1Ty(context); }
};

template<>
struct TypeGen<u128, void>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::Type::getIntNTy(context, 128); }
};

// Pointer type
template<typename T>
struct TypeGen<T*, void>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return TypeGen<T>::get(context)->getPointerTo(); }
};

// Vector type
template<typename T, int N>
struct TypeGen<T[N], void>
{
	static llvm::Type* get(llvm::LLVMContext& context) { return llvm::VectorType::get(TypeGen<T>::get(context), N); }
};

class PPUTranslator final //: public CPUTranslator
{
	// LLVM context
	llvm::LLVMContext& m_context;

	// Module to which all generated code is output to
	llvm::Module* const m_module;

	// Base address (TODO)
	const u64 m_base_addr;

	// Endianness, affects vector element numbering (TODO)
	const bool m_is_be;

	// Attributes for function calls which are "pure" and may be optimized away if their results are unused
	const llvm::AttributeSet m_pure_attr;

	// Available functions: types (not set or nullptr for untyped)
	std::unordered_map<u64, llvm::FunctionType*> m_func_types;

	// Available functions
	std::unordered_map<u64, llvm::Function*> m_func_list;

	// LLVM IR builder
	llvm::IRBuilder<>* m_ir;

	// LLVM function
	llvm::Function* m_function;

	// LLVM function type (may be null)
	llvm::FunctionType* m_function_type;

	// Function range
	u64 m_start_addr, m_end_addr, m_current_addr;

	// Basic blocks for current function
	std::unordered_map<u64, llvm::BasicBlock*> m_blocks;

	// JT resolver block
	llvm::BasicBlock* m_jtr;

	llvm::MDNode* m_md_unlikely;
	llvm::MDNode* m_md_likely;

	// Current binary data
	be_t<u32>* m_bin{};

	/* Variables */

	// Memory base
	llvm::Value* m_base;
	llvm::Value* m_base_loaded;

	// Thread context
	llvm::Value* m_thread;

	// Callable functions
	llvm::Value* m_call;

	// Thread context struct
	llvm::StructType* m_thread_type;

	llvm::Value* m_globals[96]{};
	llvm::Value** const m_g_gpr = m_globals + 0;
	llvm::Value** const m_g_fpr = m_globals + 32;
	llvm::Value** const m_g_vr = m_globals + 64;

	llvm::Value* m_locals[96]{};
	llvm::Value** const m_gpr = m_locals + 0;
	llvm::Value** const m_fpr = m_locals + 32;
	llvm::Value** const m_vr = m_locals + 64;

	llvm::Value* m_cr[32]{};
	llvm::Value* m_reg_lr;
	llvm::Value* m_reg_ctr; // CTR register (counter)
	llvm::Value* m_reg_vrsave;
	llvm::Value* m_xer_so; // XER.SO bit, summary overflow
	llvm::Value* m_xer_ov; // XER.OV bit, overflow flag
	llvm::Value* m_xer_ca; // XER.CA bit, carry flag
	llvm::Value* m_xer_count;
	llvm::Value* m_vscr_nj; // VSCR.NJ bit, non-Java mode
	llvm::Value* m_vscr_sat; // VSCR.SAT bit, sticky saturation flag

	llvm::Value* m_fpscr[32]{};
	llvm::Value* m_fpscr_fx; // bit 32 (first)
	llvm::Value* m_fpscr_ox; // bit 35 (4th)
	llvm::Value* m_fpscr_ux;
	llvm::Value* m_fpscr_zx;
	llvm::Value* m_fpscr_xx;
	llvm::Value* m_fpscr_vxsnan;
	llvm::Value* m_fpscr_vxisi;
	llvm::Value* m_fpscr_vxidi;
	llvm::Value* m_fpscr_vxzdz;
	llvm::Value* m_fpscr_vximz;
	llvm::Value* m_fpscr_vxvc;
	llvm::Value* m_fpscr_fr;
	llvm::Value* m_fpscr_fi;
	llvm::Value* m_fpscr_c;
	llvm::Value* m_fpscr_lt;
	llvm::Value* m_fpscr_gt;
	llvm::Value* m_fpscr_eq;
	llvm::Value* m_fpscr_un;
	llvm::Value* m_fpscr_reserved;
	llvm::Value* m_fpscr_vxsoft;
	llvm::Value* m_fpscr_vxsqrt;
	llvm::Value* m_fpscr_vxcvi;
	llvm::Value* m_fpscr_ve;
	llvm::Value* m_fpscr_oe;
	llvm::Value* m_fpscr_ue;
	llvm::Value* m_fpscr_ze;
	llvm::Value* m_fpscr_xe;
	llvm::Value* m_fpscr_ni;
	llvm::Value* m_fpscr_rnh; // RN high bit
	llvm::Value* m_fpscr_rnl; // RN low bit

public:

	// Change integer size for integer or integer vector type (by 2^degree)
	llvm::Type* ScaleType(llvm::Type*, s32 pow2 = 0);

	// Extend arg to double width with its copy
	llvm::Value* DuplicateExt(llvm::Value* arg);

	// Rotate arg left by n (n must be < bitwidth)
	llvm::Value* RotateLeft(llvm::Value* arg, u64 n);

	// Rotate arg left by n (n will be masked)
	llvm::Value* RotateLeft(llvm::Value* arg, llvm::Value* n);

	// Emit function call
	void CallFunction(u64 target, bool tail, llvm::Value* indirect = nullptr);

	// Set some registers to undef (after function call)
	void UndefineVolatileRegisters();

	// Load gpr
	llvm::Value* GetGpr(u32 r, u32 num_bits = 64);

	// Set gpr
	void SetGpr(u32 r, llvm::Value* value);

	// Get fpr
	llvm::Value* GetFpr(u32 r, u32 bits = 64, bool as_int = false);

	// Set fpr
	void SetFpr(u32 r, llvm::Value* val);

	// Vector register type
	enum class VrType
	{
		vi8, // i8 vector
		vi16, // i16 vector
		vi32, // i32 vector
		vf, // f32 vector
		i128, // Solid 128-bit integer
	};

	// Load vr
	llvm::Value* GetVr(u32 vr, VrType);

	// Load VRs
	template<typename... Vrs>
	std::array<llvm::Value*, sizeof...(Vrs)> GetVrs(VrType type, Vrs... regs)
	{
		static_assert(sizeof...(Vrs), "Empty VR list");
		return{ GetVr(regs, type)... };
	}

	// Set vr to the specified value
	void SetVr(u32 vr, llvm::Value*);

	// Bitcast to scalar integer value
	llvm::Value* Solid(llvm::Value*);

	// Compare value with zero constant of appropriate size
	llvm::Value* IsZero(llvm::Value*); llvm::Value* IsNotZero(llvm::Value*);

	// Compare value with all-ones constant of appropriate size
	llvm::Value* IsOnes(llvm::Value*); llvm::Value* IsNotOnes(llvm::Value*);

	// Broadcast specified value
	llvm::Value* Broadcast(llvm::Value* value, u32 count);

	// Saturate scalar or vector given the comparison operand and the extreme value to compare with (second result is the comparison result)
	std::pair<llvm::Value*, llvm::Value*> Saturate(llvm::Value* value, llvm::CmpInst::Predicate inst, llvm::Value* extreme);

	// Saturate signed value (second result is the disjunction of comparison results)
	std::pair<llvm::Value*, llvm::Value*> SaturateSigned(llvm::Value* value, u64 min, u64 max);

	// Multiply FP value or vector by the pow(2, scale)
	llvm::Value* Scale(llvm::Value* value, s32 scale);

	// Create shuffle instruction with constant args
	llvm::Value* Shuffle(llvm::Value* left, llvm::Value* right, std::initializer_list<u32> indices);

	// Create sign extension (with double size if type is nullptr)
	llvm::Value* SExt(llvm::Value* value, llvm::Type* = nullptr);

	template<std::size_t N>
	std::array<llvm::Value*, N> SExt(std::array<llvm::Value*, N> values, llvm::Type* type = nullptr)
	{
		for (std::size_t i = 0; i < N; i++) values[i] = SExt(values[i], type);
		return values;
	}

	// Create zero extension (with double size if type is nullptr)
	llvm::Value* ZExt(llvm::Value*, llvm::Type* = nullptr);

	template<std::size_t N>
	std::array<llvm::Value*, N> ZExt(std::array<llvm::Value*, N> values, llvm::Type* type = nullptr)
	{
		for (std::size_t i = 0; i < N; i++) values[i] = ZExt(values[i], type);
		return values;
	}

	// Add multiple elements
	llvm::Value* Add(std::initializer_list<llvm::Value*>);

	// Create tuncation (with half size if type is nullptr)
	llvm::Value* Trunc(llvm::Value*, llvm::Type* = nullptr);

	// Get specified CR bit
	llvm::Value* GetCrb(u32 crb);

	// Set specified CR bit
	void SetCrb(u32 crb, llvm::Value* value);

	// Set CR field, if `so` value (5th arg) is nullptr, loaded from XER.SO
	void SetCrField(u32 group, llvm::Value* lt, llvm::Value* gt, llvm::Value* eq, llvm::Value* so = nullptr);

	// Set CR field based on signed comparison
	void SetCrFieldSignedCmp(u32 n, llvm::Value* a, llvm::Value* b);

	// Set CR field based on unsigned comparison
	void SetCrFieldUnsignedCmp(u32 n, llvm::Value* a, llvm::Value* b);

	// Set FPSCR CC fields provided, optionally updating CR1
	void SetFPCC(llvm::Value* lt, llvm::Value* gt, llvm::Value* eq, llvm::Value* un, bool set_cr = false);

	// Update FPRF fields for the value, optionally updating CR1
	void SetFPRF(llvm::Value* value, bool set_cr);

	// Update FR bit
	void SetFPSCR_FR(llvm::Value* value);

	// Update FI bit (and set XX exception)
	void SetFPSCR_FI(llvm::Value* value);

	// Update sticky FPSCR exception bit, update FPSCR.FX
	void SetFPSCRException(llvm::Value* ptr, llvm::Value* value);

	// Get FPSCR bit (exception bits are cleared)
	llvm::Value* GetFPSCRBit(u32 n);

	// Set FPSCR bit
	void SetFPSCRBit(u32 n, llvm::Value*, bool update_fx);

	// Get XER.CA bit
	llvm::Value* GetCarry();

	// Set XER.CA bit
	void SetCarry(llvm::Value*);

	// Set XER.OV bit, and update XER.SO bit (|=)
	void SetOverflow(llvm::Value*);

	// Update sticky VSCR.SAT bit (|=)
	void SetSat(llvm::Value*);

	// Check condition for trap instructions
	llvm::Value* CheckTrapCondition(u32 to, llvm::Value* left, llvm::Value* right);

	// Emit trap
	llvm::Value* Trap(u64 addr);

	// Get condition for branch instructions
	llvm::Value* CheckBranchCondition(u32 bo, u32 bi);

	// Get hint for branch instructions
	llvm::MDNode* CheckBranchProbability(u32 bo);

	// Branch to next instruction if condition failed, never branch on nullptr
	void UseCondition(llvm::MDNode* hint, llvm::Value* = nullptr);

	// Get memory pointer
	llvm::Value* GetMemory(llvm::Value* addr, llvm::Type* type);

	// Read from memory
	llvm::Value* ReadMemory(llvm::Value* addr, llvm::Type* type, bool is_be = true, u32 align = 1);

	// Write to memory
	void WriteMemory(llvm::Value* addr, llvm::Value* value, bool is_be = true, u32 align = 1);

	// Convert a C++ type to an LLVM type
	template<typename T>
	llvm::Type* GetType()
	{
		return TypeGen<T>::get(m_context);
	}

	template<typename T>
	llvm::PointerType* GetPtrType()
	{
		return TypeGen<T>::get(m_context)->getPointerTo();
	}

	// Get an undefined value with specified type
	template<typename T>
	llvm::Value* GetUndef()
	{
		return llvm::UndefValue::get(GetType<T>());
	}

	// Call a function with attribute list
	template<typename... Args>
	llvm::Value* Call(llvm::Type* ret, llvm::AttributeSet attr, llvm::StringRef name, Args... args)
	{
		// Call the function
		return m_ir->CreateCall(m_module->getOrInsertFunction(name, llvm::FunctionType::get(ret, {args->getType()...}, false), attr), {args...});
	}

	// Call a function
	template<typename... Args>
	llvm::Value* Call(llvm::Type* ret, llvm::StringRef name, Args... args)
	{
		return Call(ret, llvm::AttributeSet{}, name, args...);
	}

	// Handle compilation errors
	void CompilationError(const std::string& error);

	PPUTranslator(llvm::LLVMContext& context, llvm::Module* module, u64 base);
	~PPUTranslator();

	// Get thread context struct type
	llvm::Type* GetContextType();

	// Add function
	void AddFunction(u64 addr, llvm::Function* func, llvm::FunctionType* type = nullptr);

	// Parses PPU opcodes and translate them into LLVM IR
	llvm::Function* TranslateToIR(const ppu_function& info, be_t<u32>* bin, void(*custom)(PPUTranslator*) = nullptr);

	void MFVSCR(ppu_opcode_t op);
	void MTVSCR(ppu_opcode_t op);
	void VADDCUW(ppu_opcode_t op);
	void VADDFP(ppu_opcode_t op);
	void VADDSBS(ppu_opcode_t op);
	void VADDSHS(ppu_opcode_t op);
	void VADDSWS(ppu_opcode_t op);
	void VADDUBM(ppu_opcode_t op);
	void VADDUBS(ppu_opcode_t op);
	void VADDUHM(ppu_opcode_t op);
	void VADDUHS(ppu_opcode_t op);
	void VADDUWM(ppu_opcode_t op);
	void VADDUWS(ppu_opcode_t op);
	void VAND(ppu_opcode_t op);
	void VANDC(ppu_opcode_t op);
	void VAVGSB(ppu_opcode_t op);
	void VAVGSH(ppu_opcode_t op);
	void VAVGSW(ppu_opcode_t op);
	void VAVGUB(ppu_opcode_t op);
	void VAVGUH(ppu_opcode_t op);
	void VAVGUW(ppu_opcode_t op);
	void VCFSX(ppu_opcode_t op);
	void VCFUX(ppu_opcode_t op);
	void VCMPBFP(ppu_opcode_t op);
	void VCMPEQFP(ppu_opcode_t op);
	void VCMPEQUB(ppu_opcode_t op);
	void VCMPEQUH(ppu_opcode_t op);
	void VCMPEQUW(ppu_opcode_t op);
	void VCMPGEFP(ppu_opcode_t op);
	void VCMPGTFP(ppu_opcode_t op);
	void VCMPGTSB(ppu_opcode_t op);
	void VCMPGTSH(ppu_opcode_t op);
	void VCMPGTSW(ppu_opcode_t op);
	void VCMPGTUB(ppu_opcode_t op);
	void VCMPGTUH(ppu_opcode_t op);
	void VCMPGTUW(ppu_opcode_t op);
	void VCTSXS(ppu_opcode_t op);
	void VCTUXS(ppu_opcode_t op);
	void VEXPTEFP(ppu_opcode_t op);
	void VLOGEFP(ppu_opcode_t op);
	void VMADDFP(ppu_opcode_t op);
	void VMAXFP(ppu_opcode_t op);
	void VMAXSB(ppu_opcode_t op);
	void VMAXSH(ppu_opcode_t op);
	void VMAXSW(ppu_opcode_t op);
	void VMAXUB(ppu_opcode_t op);
	void VMAXUH(ppu_opcode_t op);
	void VMAXUW(ppu_opcode_t op);
	void VMHADDSHS(ppu_opcode_t op);
	void VMHRADDSHS(ppu_opcode_t op);
	void VMINFP(ppu_opcode_t op);
	void VMINSB(ppu_opcode_t op);
	void VMINSH(ppu_opcode_t op);
	void VMINSW(ppu_opcode_t op);
	void VMINUB(ppu_opcode_t op);
	void VMINUH(ppu_opcode_t op);
	void VMINUW(ppu_opcode_t op);
	void VMLADDUHM(ppu_opcode_t op);
	void VMRGHB(ppu_opcode_t op);
	void VMRGHH(ppu_opcode_t op);
	void VMRGHW(ppu_opcode_t op);
	void VMRGLB(ppu_opcode_t op);
	void VMRGLH(ppu_opcode_t op);
	void VMRGLW(ppu_opcode_t op);
	void VMSUMMBM(ppu_opcode_t op);
	void VMSUMSHM(ppu_opcode_t op);
	void VMSUMSHS(ppu_opcode_t op);
	void VMSUMUBM(ppu_opcode_t op);
	void VMSUMUHM(ppu_opcode_t op);
	void VMSUMUHS(ppu_opcode_t op);
	void VMULESB(ppu_opcode_t op);
	void VMULESH(ppu_opcode_t op);
	void VMULEUB(ppu_opcode_t op);
	void VMULEUH(ppu_opcode_t op);
	void VMULOSB(ppu_opcode_t op);
	void VMULOSH(ppu_opcode_t op);
	void VMULOUB(ppu_opcode_t op);
	void VMULOUH(ppu_opcode_t op);
	void VNMSUBFP(ppu_opcode_t op);
	void VNOR(ppu_opcode_t op);
	void VOR(ppu_opcode_t op);
	void VPERM(ppu_opcode_t op);
	void VPKPX(ppu_opcode_t op);
	void VPKSHSS(ppu_opcode_t op);
	void VPKSHUS(ppu_opcode_t op);
	void VPKSWSS(ppu_opcode_t op);
	void VPKSWUS(ppu_opcode_t op);
	void VPKUHUM(ppu_opcode_t op);
	void VPKUHUS(ppu_opcode_t op);
	void VPKUWUM(ppu_opcode_t op);
	void VPKUWUS(ppu_opcode_t op);
	void VREFP(ppu_opcode_t op);
	void VRFIM(ppu_opcode_t op);
	void VRFIN(ppu_opcode_t op);
	void VRFIP(ppu_opcode_t op);
	void VRFIZ(ppu_opcode_t op);
	void VRLB(ppu_opcode_t op);
	void VRLH(ppu_opcode_t op);
	void VRLW(ppu_opcode_t op);
	void VRSQRTEFP(ppu_opcode_t op);
	void VSEL(ppu_opcode_t op);
	void VSL(ppu_opcode_t op);
	void VSLB(ppu_opcode_t op);
	void VSLDOI(ppu_opcode_t op);
	void VSLH(ppu_opcode_t op);
	void VSLO(ppu_opcode_t op);
	void VSLW(ppu_opcode_t op);
	void VSPLTB(ppu_opcode_t op);
	void VSPLTH(ppu_opcode_t op);
	void VSPLTISB(ppu_opcode_t op);
	void VSPLTISH(ppu_opcode_t op);
	void VSPLTISW(ppu_opcode_t op);
	void VSPLTW(ppu_opcode_t op);
	void VSR(ppu_opcode_t op);
	void VSRAB(ppu_opcode_t op);
	void VSRAH(ppu_opcode_t op);
	void VSRAW(ppu_opcode_t op);
	void VSRB(ppu_opcode_t op);
	void VSRH(ppu_opcode_t op);
	void VSRO(ppu_opcode_t op);
	void VSRW(ppu_opcode_t op);
	void VSUBCUW(ppu_opcode_t op);
	void VSUBFP(ppu_opcode_t op);
	void VSUBSBS(ppu_opcode_t op);
	void VSUBSHS(ppu_opcode_t op);
	void VSUBSWS(ppu_opcode_t op);
	void VSUBUBM(ppu_opcode_t op);
	void VSUBUBS(ppu_opcode_t op);
	void VSUBUHM(ppu_opcode_t op);
	void VSUBUHS(ppu_opcode_t op);
	void VSUBUWM(ppu_opcode_t op);
	void VSUBUWS(ppu_opcode_t op);
	void VSUMSWS(ppu_opcode_t op);
	void VSUM2SWS(ppu_opcode_t op);
	void VSUM4SBS(ppu_opcode_t op);
	void VSUM4SHS(ppu_opcode_t op);
	void VSUM4UBS(ppu_opcode_t op);
	void VUPKHPX(ppu_opcode_t op);
	void VUPKHSB(ppu_opcode_t op);
	void VUPKHSH(ppu_opcode_t op);
	void VUPKLPX(ppu_opcode_t op);
	void VUPKLSB(ppu_opcode_t op);
	void VUPKLSH(ppu_opcode_t op);
	void VXOR(ppu_opcode_t op);

	void TDI(ppu_opcode_t op);
	void TWI(ppu_opcode_t op);
	void MULLI(ppu_opcode_t op);
	void SUBFIC(ppu_opcode_t op);
	void CMPLI(ppu_opcode_t op);
	void CMPI(ppu_opcode_t op);
	void ADDIC(ppu_opcode_t op);
	void ADDI(ppu_opcode_t op);
	void ADDIS(ppu_opcode_t op);
	void BC(ppu_opcode_t op);
	void HACK(ppu_opcode_t op);
	void SC(ppu_opcode_t op);
	void B(ppu_opcode_t op);
	void MCRF(ppu_opcode_t op);
	void BCLR(ppu_opcode_t op);
	void CRNOR(ppu_opcode_t op);
	void CRANDC(ppu_opcode_t op);
	void ISYNC(ppu_opcode_t op);
	void CRXOR(ppu_opcode_t op);
	void CRNAND(ppu_opcode_t op);
	void CRAND(ppu_opcode_t op);
	void CREQV(ppu_opcode_t op);
	void CRORC(ppu_opcode_t op);
	void CROR(ppu_opcode_t op);
	void BCCTR(ppu_opcode_t op);
	void RLWIMI(ppu_opcode_t op);
	void RLWINM(ppu_opcode_t op);
	void RLWNM(ppu_opcode_t op);
	void ORI(ppu_opcode_t op);
	void ORIS(ppu_opcode_t op);
	void XORI(ppu_opcode_t op);
	void XORIS(ppu_opcode_t op);
	void ANDI(ppu_opcode_t op);
	void ANDIS(ppu_opcode_t op);
	void RLDICL(ppu_opcode_t op);
	void RLDICR(ppu_opcode_t op);
	void RLDIC(ppu_opcode_t op);
	void RLDIMI(ppu_opcode_t op);
	void RLDCL(ppu_opcode_t op);
	void RLDCR(ppu_opcode_t op);
	void CMP(ppu_opcode_t op);
	void TW(ppu_opcode_t op);
	void LVSL(ppu_opcode_t op);
	void LVEBX(ppu_opcode_t op);
	void SUBFC(ppu_opcode_t op);
	void MULHDU(ppu_opcode_t op);
	void ADDC(ppu_opcode_t op);
	void MULHWU(ppu_opcode_t op);
	void MFOCRF(ppu_opcode_t op);
	void LWARX(ppu_opcode_t op);
	void LDX(ppu_opcode_t op);
	void LWZX(ppu_opcode_t op);
	void SLW(ppu_opcode_t op);
	void CNTLZW(ppu_opcode_t op);
	void SLD(ppu_opcode_t op);
	void AND(ppu_opcode_t op);
	void CMPL(ppu_opcode_t op);
	void LVSR(ppu_opcode_t op);
	void LVEHX(ppu_opcode_t op);
	void SUBF(ppu_opcode_t op);
	void LDUX(ppu_opcode_t op);
	void DCBST(ppu_opcode_t op);
	void LWZUX(ppu_opcode_t op);
	void CNTLZD(ppu_opcode_t op);
	void ANDC(ppu_opcode_t op);
	void TD(ppu_opcode_t op);
	void LVEWX(ppu_opcode_t op);
	void MULHD(ppu_opcode_t op);
	void MULHW(ppu_opcode_t op);
	void LDARX(ppu_opcode_t op);
	void DCBF(ppu_opcode_t op);
	void LBZX(ppu_opcode_t op);
	void LVX(ppu_opcode_t op);
	void NEG(ppu_opcode_t op);
	void LBZUX(ppu_opcode_t op);
	void NOR(ppu_opcode_t op);
	void STVEBX(ppu_opcode_t op);
	void SUBFE(ppu_opcode_t op);
	void ADDE(ppu_opcode_t op);
	void MTOCRF(ppu_opcode_t op);
	void STDX(ppu_opcode_t op);
	void STWCX(ppu_opcode_t op);
	void STWX(ppu_opcode_t op);
	void STVEHX(ppu_opcode_t op);
	void STDUX(ppu_opcode_t op);
	void STWUX(ppu_opcode_t op);
	void STVEWX(ppu_opcode_t op);
	void SUBFZE(ppu_opcode_t op);
	void ADDZE(ppu_opcode_t op);
	void STDCX(ppu_opcode_t op);
	void STBX(ppu_opcode_t op);
	void STVX(ppu_opcode_t op);
	void MULLD(ppu_opcode_t op);
	void SUBFME(ppu_opcode_t op);
	void ADDME(ppu_opcode_t op);
	void MULLW(ppu_opcode_t op);
	void DCBTST(ppu_opcode_t op);
	void STBUX(ppu_opcode_t op);
	void ADD(ppu_opcode_t op);
	void DCBT(ppu_opcode_t op);
	void LHZX(ppu_opcode_t op);
	void EQV(ppu_opcode_t op);
	void ECIWX(ppu_opcode_t op);
	void LHZUX(ppu_opcode_t op);
	void XOR(ppu_opcode_t op);
	void MFSPR(ppu_opcode_t op);
	void LWAX(ppu_opcode_t op);
	void DST(ppu_opcode_t op);
	void LHAX(ppu_opcode_t op);
	void LVXL(ppu_opcode_t op);
	void MFTB(ppu_opcode_t op);
	void LWAUX(ppu_opcode_t op);
	void DSTST(ppu_opcode_t op);
	void LHAUX(ppu_opcode_t op);
	void STHX(ppu_opcode_t op);
	void ORC(ppu_opcode_t op);
	void ECOWX(ppu_opcode_t op);
	void STHUX(ppu_opcode_t op);
	void OR(ppu_opcode_t op);
	void DIVDU(ppu_opcode_t op);
	void DIVWU(ppu_opcode_t op);
	void MTSPR(ppu_opcode_t op);
	void DCBI(ppu_opcode_t op);
	void NAND(ppu_opcode_t op);
	void STVXL(ppu_opcode_t op);
	void DIVD(ppu_opcode_t op);
	void DIVW(ppu_opcode_t op);
	void LVLX(ppu_opcode_t op);
	void LDBRX(ppu_opcode_t op);
	void LSWX(ppu_opcode_t op);
	void LWBRX(ppu_opcode_t op);
	void LFSX(ppu_opcode_t op);
	void SRW(ppu_opcode_t op);
	void SRD(ppu_opcode_t op);
	void LVRX(ppu_opcode_t op);
	void LSWI(ppu_opcode_t op);
	void LFSUX(ppu_opcode_t op);
	void SYNC(ppu_opcode_t op);
	void LFDX(ppu_opcode_t op);
	void LFDUX(ppu_opcode_t op);
	void STVLX(ppu_opcode_t op);
	void STDBRX(ppu_opcode_t op);
	void STSWX(ppu_opcode_t op);
	void STWBRX(ppu_opcode_t op);
	void STFSX(ppu_opcode_t op);
	void STVRX(ppu_opcode_t op);
	void STFSUX(ppu_opcode_t op);
	void STSWI(ppu_opcode_t op);
	void STFDX(ppu_opcode_t op);
	void STFDUX(ppu_opcode_t op);
	void LVLXL(ppu_opcode_t op);
	void LHBRX(ppu_opcode_t op);
	void SRAW(ppu_opcode_t op);
	void SRAD(ppu_opcode_t op);
	void LVRXL(ppu_opcode_t op);
	void DSS(ppu_opcode_t op);
	void SRAWI(ppu_opcode_t op);
	void SRADI(ppu_opcode_t op);
	void EIEIO(ppu_opcode_t op);
	void STVLXL(ppu_opcode_t op);
	void STHBRX(ppu_opcode_t op);
	void EXTSH(ppu_opcode_t op);
	void STVRXL(ppu_opcode_t op);
	void EXTSB(ppu_opcode_t op);
	void STFIWX(ppu_opcode_t op);
	void EXTSW(ppu_opcode_t op);
	void ICBI(ppu_opcode_t op);
	void DCBZ(ppu_opcode_t op);
	void LWZ(ppu_opcode_t op);
	void LWZU(ppu_opcode_t op);
	void LBZ(ppu_opcode_t op);
	void LBZU(ppu_opcode_t op);
	void STW(ppu_opcode_t op);
	void STWU(ppu_opcode_t op);
	void STB(ppu_opcode_t op);
	void STBU(ppu_opcode_t op);
	void LHZ(ppu_opcode_t op);
	void LHZU(ppu_opcode_t op);
	void LHA(ppu_opcode_t op);
	void LHAU(ppu_opcode_t op);
	void STH(ppu_opcode_t op);
	void STHU(ppu_opcode_t op);
	void LMW(ppu_opcode_t op);
	void STMW(ppu_opcode_t op);
	void LFS(ppu_opcode_t op);
	void LFSU(ppu_opcode_t op);
	void LFD(ppu_opcode_t op);
	void LFDU(ppu_opcode_t op);
	void STFS(ppu_opcode_t op);
	void STFSU(ppu_opcode_t op);
	void STFD(ppu_opcode_t op);
	void STFDU(ppu_opcode_t op);
	void LD(ppu_opcode_t op);
	void LDU(ppu_opcode_t op);
	void LWA(ppu_opcode_t op);
	void STD(ppu_opcode_t op);
	void STDU(ppu_opcode_t op);

	void FDIVS(ppu_opcode_t op);
	void FSUBS(ppu_opcode_t op);
	void FADDS(ppu_opcode_t op);
	void FSQRTS(ppu_opcode_t op);
	void FRES(ppu_opcode_t op);
	void FMULS(ppu_opcode_t op);
	void FMADDS(ppu_opcode_t op);
	void FMSUBS(ppu_opcode_t op);
	void FNMSUBS(ppu_opcode_t op);
	void FNMADDS(ppu_opcode_t op);
	void MTFSB1(ppu_opcode_t op);
	void MCRFS(ppu_opcode_t op);
	void MTFSB0(ppu_opcode_t op);
	void MTFSFI(ppu_opcode_t op);
	void MFFS(ppu_opcode_t op);
	void MTFSF(ppu_opcode_t op);
	void FCMPU(ppu_opcode_t op);
	void FRSP(ppu_opcode_t op);
	void FCTIW(ppu_opcode_t op);
	void FCTIWZ(ppu_opcode_t op);
	void FDIV(ppu_opcode_t op);
	void FSUB(ppu_opcode_t op);
	void FADD(ppu_opcode_t op);
	void FSQRT(ppu_opcode_t op);
	void FSEL(ppu_opcode_t op);
	void FMUL(ppu_opcode_t op);
	void FRSQRTE(ppu_opcode_t op);
	void FMSUB(ppu_opcode_t op);
	void FMADD(ppu_opcode_t op);
	void FNMSUB(ppu_opcode_t op);
	void FNMADD(ppu_opcode_t op);
	void FCMPO(ppu_opcode_t op);
	void FNEG(ppu_opcode_t op);
	void FMR(ppu_opcode_t op);
	void FNABS(ppu_opcode_t op);
	void FABS(ppu_opcode_t op);
	void FCTID(ppu_opcode_t op);
	void FCTIDZ(ppu_opcode_t op);
	void FCFID(ppu_opcode_t op);

	void UNK(ppu_opcode_t op);
};

#endif
