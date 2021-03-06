//===---- SSAPRELegacy.cpp - SSA PARTIAL REDUNDANCY ELIMINATION -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The pass is based on paper "A new algorithm for partial redundancy
// elimination based on SSA form" by Fred Chow, Sun Chan, Robert Kennedy
// Shin-Ming Liu, Raymond Lo and Peng Tu.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/SSAPRE.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BreakCriticalEdges.h"
#include "llvm/Analysis/IteratedDominanceFrontier.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/CFG.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::ssapre;

#define DEBUG_TYPE "ssapre"

STATISTIC(SSAPREInstrSubstituted,  "Number of instructions substituted");
STATISTIC(SSAPREInstrInserted,     "Number of instructions inserted");
STATISTIC(SSAPREInstrKilled,       "Number of instructions deleted");
STATISTIC(SSAPREPHIInserted,       "Number of phi inserted");
STATISTIC(SSAPREPHIKilled,         "Number of phi deleted");
STATISTIC(SSAPREBlah,              "Blah");

// Anchor methods.
namespace llvm {
namespace ssapre {
Expression::~Expression() = default;
IgnoredExpression::~IgnoredExpression() = default;
UnknownExpression::~UnknownExpression() = default;
BasicExpression::~BasicExpression() = default;
PHIExpression::~PHIExpression() = default;
FactorExpression::~FactorExpression() = default;
}
}

//===----------------------------------------------------------------------===//
// Utility
//===----------------------------------------------------------------------===//

static bool
IsVersionUnset(const Expression *E) {
  return E->getVersion() == VR_Unset;
}

static Expression TExpr(ET_Top,    ~2U, VR_Top);
static Expression BExpr(ET_Bottom, ~2U, VR_Bottom);
static Expression * GetTop()    { return &TExpr; }
static Expression * GetBottom() { return &BExpr; }

bool SSAPRE::
IsTop(const Expression *E) {
  assert(E);
  return E == GetTop();
}

bool SSAPRE::
IsBottom(const Expression *E) {
  assert(E);
  return E == GetBottom();
}

bool SSAPRE::
IsBottomOrVarOrConst(const Expression *E) {
  assert(E);
  return E == GetBottom() || IsVariableOrConstant(E);
}

ExpVector_t & SSAPRE::
GetSameVExpr(const FactorExpression *F) {
  assert(F && !IsBottomOrVarOrConst(F));
  auto PE = F->getPExpr();
  assert(PE && !IsBottomOrVarOrConst(PE));
  return PExprToVersions[PE][F->getVersion()];
}

ExpVector_t & SSAPRE::
GetSameVExpr(const Expression *E) {
  assert(E && !IsBottom(E));
  auto PE = ExprToPExpr[E];
  assert(PE && !IsBottom(PE));
  return PExprToVersions[PE][E->getVersion()];
}

bool SSAPRE::
IsVariableOrConstant(const Expression *E) {
  assert(E);
  return E->getExpressionType() == ET_Variable ||
         E->getExpressionType() == ET_Constant;
}

bool SSAPRE::
IsVariableOrConstant(const Value *V) {
  assert(V);
  return Argument::classof(V) ||
         GlobalValue::classof(V) ||
         Constant::classof(V);
}

bool SSAPRE::
IsFactoredPHI(Instruction *I) {
  assert(I);
  if (auto PHI = dyn_cast<PHINode>(I)) {
    return PHIToFactor[PHI];
  }
  return false;
}

const Instruction * SSAPRE::
GetDomRepresentativeInstruction(const Expression * E) {
  // There is a certain dominance trickery with factored and non-factored PHIs.
  // The factored PHIs always dominate non-factored ones, in this regard plain
  // PHIs treated as a regular instructions.
  if (auto FE = dyn_cast<FactorExpression>(E)) {
    auto BB = (BasicBlock *)FactorToBlock[FE];
    auto DN = DT->getNode(BB);
    auto PB = DN->getIDom()->getBlock();
    return PB->getTerminator();
  } else if (auto PHIE = dyn_cast<PHIExpression>(E)) {
    auto BB = VExprToInst[PHIE]->getParent();
    return &BB->front();
  }

  return VExprToInst[E];
}

bool SSAPRE::
StrictlyDominates(const Expression *Def, const Expression *Use) {
  assert (Def && Use && "Def or Use is null");

  auto IDef = GetDomRepresentativeInstruction(Def);
  auto IUse = GetDomRepresentativeInstruction(Use);

  assert (IDef && IUse && "IDef or IUse is null");

  // Strictly
  if (IDef == IUse) return false;

  return DT->dominates(IDef, IUse);
}

bool SSAPRE::
NotStrictlyDominates(const Expression *Def, const Expression *Use) {
  assert (Def && Use && "Def or Use is null");

  auto IDef = GetDomRepresentativeInstruction(Def);
  auto IUse = GetDomRepresentativeInstruction(Use);

  assert (IDef && IUse && "IDef or IUse is null");

  // Not Strictly
  if (IDef == IUse) return true;

  return DT->dominates(IDef, IUse);
}

bool SSAPRE::
OperandsDominate(const Expression *Def, const Expression *Use) {
  return OperandsDominate(VExprToInst[Def], Use);
}

bool SSAPRE::
OperandsDominate(const Instruction *I, const Expression *Use) {
  for (auto &O : I->operands()) {
    auto E = ValueToExp[O];

    // Variables or Constants occurs indefinitely before any expression
    if (IsVariableOrConstant(E)) continue;

    // We want to use the earliest occurrence of the operand, it will be either
    // a Factor, another definition or the same definition if it defines a new
    // version.
    E = GetSubstitution(E);

    if (IsVariableOrConstant(E)) continue;

    // Due to the way we check dominance for factors we need to use non-strict
    // dominance if both operands a factors
    if (!NotStrictlyDominates(E, Use)) return false;
  }

  return true;
}

bool SSAPRE::
OperandsDominateStrictly(const Expression *Def, const Expression *Use) {
  return OperandsDominateStrictly(VExprToInst[Def], Use);
}

bool SSAPRE::
OperandsDominateStrictly(const Instruction *I, const Expression *Use) {
  for (auto &O : I->operands()) {
    auto E = ValueToExp[O];

    // Variables or Constants occurs indefinitely before any expression
    if (IsVariableOrConstant(E)) continue;

    // We want to use the earliest occurrence of the operand, it will be either
    // a Factor, another definition or the same definition if it defines a new
    // version.
    E = GetSubstitution(E);

    if (IsVariableOrConstant(E)) continue;

    if (!StrictlyDominates(E, Use)) return false;
  }

  return true;
}

bool SSAPRE::
HasRealUseBefore(const Expression *S, const BBVector_t &P,
                 const Expression *E) {
  auto EDFS = InstrDFS[VExprToInst[E]];

  // We need to check every expression that shares the same version
  for (auto V : GetSameVExpr(S)) {
    for (auto U : VExprToInst[V]->users()) {
      auto UI = (Instruction *)U;

      // Ignore PHIs that are linked with Factors, since those bonds solved
      // through the main algorithm
      if (IsFactoredPHI(UI)) continue;

      auto UB = UI->getParent();
      for (auto PB : P) {
        // User is on the Path and it happens before E
        if (UB == PB && InstrDFS[UI] <= EDFS) return true;
      }
    }
  }

  return false;
}

bool SSAPRE::
FactorHasRealUseBefore(const FactorExpression *F, const BBVector_t &P,
                       const Expression *E) {
  auto EDFS = InstrDFS[VExprToInst[E]];

  // If Factor is linked with a PHI we need to check its users.
  if (auto PHI = FactorToPHI[F]) {
    for (auto U : PHI->users()) {
      auto UI = (Instruction *)U;

      // Ignore PHIs that are linked with Factors, since those bonds solved
      // through the main algorithm
      if (IsFactoredPHI(UI)) continue;

      auto UB = UI->getParent();
      for (auto PB : P) {
        // User is on the Path and it happens before E
        if (UB == PB && InstrDFS[UI] <= EDFS) return true;
      }
    }
  }

  // We check every Expression of the same version as the Factor we check,
  // since by definition those will come after the Factor
  for (auto V : GetSameVExpr(F)) {
    for (auto U : VExprToInst[V]->users()) {
      auto UI = (Instruction *)U;

      // Ignore PHIs that are linked with Factors, since those bonds solved
      // through the main algorithm
      if (IsFactoredPHI(UI)) continue;

      auto UB = UI->getParent();
      for (auto PB : P) {
        // User is on the Path and it happens before E
        if (UB == PB && InstrDFS[UI] <= EDFS) return true;
      }
    }
  }

  return false;
}

bool SSAPRE::
IgnoreExpression(const Expression *E) {
  assert(E);
  auto ET = E->getExpressionType();
  return ET == ET_Ignored  ||
         ET == ET_Unknown  ||
         ET == ET_Variable ||
         ET == ET_Constant;
}

bool SSAPRE::
IsToBeKilled(Expression *E) {
  assert(E);
  auto V = ExpToValue[E];
  assert(V);
  for (auto K : KillList) {
    if (V == K) return true;
  }
  return false;
}

bool SSAPRE::
IsToBeKilled(Instruction *I) {
  assert(I);
  for (auto K : KillList) {
    if (I == K) return true;
  }
  return false;
}

bool SSAPRE::
AllUsersKilled(const Instruction *I) {
  assert(I);
  for (auto U : I->users()) {
    auto UI = (Instruction *)U;
    if (UI->getParent()) {
      bool Killed = false;
      for (auto K : KillList) {
        if (K == UI) {
          Killed = true;
          break;
        }
      }

      if (!Killed) return false;
    }
  }
  return true;
}

void SSAPRE::
SetOrderBefore(Instruction *I, Instruction *B) {
  assert(I && B);
  InstrSDFS[I] = InstrSDFS[B]; InstrSDFS[B]++;
  InstrDFS[I]  = InstrDFS[B];  InstrDFS[B]++;
}

void SSAPRE::
SetAllOperandsSave(Instruction *I) {
  assert(I);
  for (auto &U : I->operands()) {
    auto UE = ValueToExp[U];
    UE->addSave();
  }
}

void SSAPRE::
AddSubstitution(Expression *E, Expression *S, bool Direct, bool Force) {
  assert(E && S);
  assert((Force ||
      ExprToPExpr[E] == ExprToPExpr[S] ||
      IsBottomOrVarOrConst(S) ||
      IsTop(S)) &&
      "Substituting expression must be of the same Proto or Top or Bottom");


  auto *PE = ExprToPExpr[E];
  if (!PE) PE = E;

  assert(PE);

  if (!Substitutions.count(PE)) {
    Substitutions.insert({PE, ExpExpMap()});
  }

  auto &MA = Substitutions[PE];

  if (E == S) {
    MA[E] = S;
    return;
  }

  if (!Direct) {
    // Try get the last one
    if (auto SS = GetSubstitution(S)) S = SS;
  }

  if (
      // Any F -> E substitution serves as a jump record
      !FactorExpression::classof(E) &&
      // Only if this is the first time we add this substitution
      MA[E] != S) {
    S->addSave();
  }

  assert(S);
  MA[E] = S;
}

Expression * SSAPRE::
GetSubstitution(Expression *E, bool Direct) {
  assert(E);

  if (IsBottomOrVarOrConst(E) || IsTop(E)) return E;

  auto PE = ExprToPExpr[E];
  if (!PE) PE = E;
  if (!Substitutions.count(PE)) {
    llvm_unreachable("This type of expressions does not exist in the record");
  }
  auto &MA = Substitutions[PE];

  if (Direct) {
    auto S = MA[E];
    if (S) return S;
    return E;
  }

  while (auto EE = GetSubstitution(E, true)) {
    assert(EE && "Substitution cannot be null");
    if (IsBottom(EE) || IsTop(EE)) return EE;
    if (E == EE) return E;
    E = EE;
  }

  return E;
}

void SSAPRE::
RemSubstitution(Expression *E) {
  assert(E);

  auto PE = ExprToPExpr[E];
  assert(PE);

  if (Substitutions.count(PE))
    Substitutions[PE].erase(E);
}

Value * SSAPRE::
GetSubstituteValue(Expression *E) {
  E = GetSubstitution(E);
  if (auto F = dyn_cast<FactorExpression>(E)) {
    if (F->getIsMaterialized())
      return (Value *)FactorToPHI[F];
    else
      llvm_unreachable("Must not have happened");
  }
  return (Value *)ExpToValue[E];
}

void SSAPRE::
AddConstant(ConstantExpression *CE, Constant *C) {
  assert(CE && C);
  ExpToValue[CE] = C;
  ValueToExp[C] = CE;
  COExpToValue[CE] = C;
  ValueToCOExp[C] = CE;
}

void SSAPRE::
AddExpression(Expression *PE, Expression *VE, Instruction *I, BasicBlock *B) {
  assert(PE && VE && I && B);

  ExpToValue[VE] = I;
  ValueToExp[I] = VE;

  InstToVExpr[I] = VE;
  VExprToInst[VE] = I;
  ExprToPExpr[VE] = PE;

  if (!PExprToVExprs.count(PE)) {
    PExprToVExprs.insert({PE, {VE}});
  } else {
    PExprToVExprs[PE].insert(VE);
  }

  if (!PExprToInsts.count(PE)) {
    PExprToInsts.insert({PE, {I}});
  } else {
    PExprToInsts[PE].insert(I);
  }

  if (!PExprToBlocks.count(PE)) {
    PExprToBlocks.insert({PE, {B}});
  } else {
    PExprToBlocks[PE].insert(B);
  }

  // Must be the last
  AddSubstitution(VE, VE);
}

void SSAPRE::
AddFactor(FactorExpression *FE, const Expression *PE, const BasicBlock *B) {
  assert(FE && PE && B);
  assert(FE != PE);
  FE->setPExpr(PE);
  ExprToPExpr[FE] = PE;
  FactorToBlock[FE] = B;
  BlockToFactors[B].push_back(FE);
  FExprs.insert(FE);

  // Must be the last
  AddSubstitution(FE, FE);
}

void SSAPRE::
KillFactor(FactorExpression *F, bool BottomSubstitute) {
  assert(F);

  // Must be the first
  if (BottomSubstitute) AddSubstitution(F, GetBottom());

  auto &B = FactorToBlock[F];
  auto &V = BlockToFactors[B];
  for (auto VS = V.begin(), VV = V.end(); VS != VV; ++VS) {
    if (*VS == F) V.erase(VS);
  }

  FactorToBlock.erase(F);
  FExprs.erase(F);
  VExprToInst.erase(F);
  ExpToValue.erase(F);
  // ExprToPExpr.erase(F);

  if (F->getIsMaterialized()) {
    F->setIsMaterialized(false);
    auto PHI = (PHINode *)FactorToPHI[F];
    PHIToFactor[PHI] = nullptr;
    FactorToPHI[F] = nullptr;

    // Replace the FactorExpression with a regular PHIExpression
    auto E = CreateExpression(*PHI);
    auto P = CreateExpression(*PHI);
    AddExpression(P, E, PHI, PHI->getParent());
  }
}

void SSAPRE::
MaterializeFactor(FactorExpression *FE, PHINode *PHI) {
  assert(FE && PHI);

  FE->setIsMaterialized(true);

  // These may not exist if we just materialized the phi
  auto PVE = InstToVExpr[PHI];
  auto PPE = ExprToPExpr[PVE];

  if (PPE) {
    // We need to remove anything related to this PHIs original prototype,
    // because before we verified that this PHI is actually a Factor it was based
    // on its own PHI proto instance.
    PExprToVExprs.erase(PPE);
    PExprToInsts.erase(PPE);
    PExprToBlocks.erase(PPE);
    PExprToVersions.erase(PPE);

    PPE->getProto()->dropAllReferences();
    ExpressionAllocator.Deallocate(PPE);
  }

  if (PVE) {
    RemSubstitution(PVE);

    // Erase all memory of it
    ExpToValue.erase(PVE);
    VExprToInst.erase(PVE);
    ExprToPExpr.erase(PVE);

    // If there is a Factor that uses this PHI as operand
    for (auto F : FExprs) {
      if (F->hasVExpr(PVE))
        F->replaceVExpr(PVE, FE);
    }

    ExpressionAllocator.Deallocate(PVE);
  }

  // Wire FE to PHI
  FactorToPHI[FE] = PHI;
  PHIToFactor[PHI] = FE;

  InstToVExpr[PHI] = FE;
  VExprToInst[FE] = PHI;
  // ExprToPExpr[FE] = FE;

  ExpToValue[FE] = PHI;
  ValueToExp[PHI] = FE;
}

bool SSAPRE::
ReplaceFactor(FactorExpression *FE, Expression *VE, bool HRU, bool Direct) {
  if (FE->getIsMaterialized()) {
    ReplaceFactorMaterialized(FE, VE, HRU, Direct);
    return true;
  }

  ReplaceFactorFinalize(FE, VE, HRU, Direct);
  return false;
}

void SSAPRE::
ReplaceFactorMaterialized(FactorExpression * FE, Expression * VE,
                          bool HRU, bool Direct) {
  assert(FE && VE);

  // We want the most recent expression
  if (!Direct) VE = GetSubstitution(VE);

  bool IsTopOrBot = IsTop(VE) || IsBottom(VE);

  // Add save for every real use of this PHI
  auto PHI = (PHINode *)FactorToPHI[FE];

  for (auto U : PHI->users()) {

    auto UI = (Instruction *)U;
    auto UE  = InstToVExpr[UI];

    // Skip instruction without parents
    if (!UI->getParent()) continue;

    if (IsTopOrBot && !IsToBeKilled (UI) && !FactorExpression::classof(UE)) {
      llvm_unreachable("You cannot replace Factor with Bottom \
                        for a regular non-factored instruction");
    }

    VE->addSave();
  }

  // Replace all PHI uses with a real instruction result only
  if (!IsTopOrBot &&
      !(FactorExpression::classof(VE) && !FactorToPHI[(FactorExpression*)VE])) {
    auto V = (Value *)ExpToValue[VE];
    PHI->replaceAllUsesWith(V);
    SSAPREInstrSubstituted++;
  }

  FE->setIsMaterialized(false);
  PHIToFactor[PHI] = nullptr;
  FactorToPHI[FE] = nullptr;

  KillList.push_back(PHI);

  // The rest is the same as for non-materialized Factor
  ReplaceFactorFinalize(FE, VE, HRU, Direct);
}

void SSAPRE::
ReplaceFactorFinalize(FactorExpression *FE, Expression *VE,
                      bool HRU, bool Direct) {
  assert(FE && VE);

  // We want the most recent expression
  if (!Direct)
    VE = GetSubstitution(VE);

  // Replace all Factor uses. Note that we do not add Save for each Factor use,
  // because Factors do not use their operands before they're materialized, or
  // in case of already materialized not-removed during CodeMotion step.
  auto List = FExprs; // Can be modified insdie the cycle
  for (auto F : List) {
    if (F == FE) continue;
    for (auto BB : F->getPreds()) {
      if (F->getVExpr(BB) != FE) continue;

      F->setVExpr(BB, VE);
      F->setHasRealUse(VE, HRU);

      // If we assign the same version we create a cycle
      if (F->getVersion() == VE->getVersion()) {

        // Assigning this VE as operand makes it induction expression, yikes.
        // In this case just kill this F right away
        if (IsInductionExpression(F, VE)) {
          KillFactor(F);
        } else {
          F->setIsCycle(VE, true);
        }
      }
    }
  }

  // Any Expression of the same type and version follows this Factor occurrence
  // by definition, since we replace the factor with another Expression we can
  // remove all other expressions of the same version and replace their usage
  // with this new one.
  for (auto V : GetSameVExpr(FE)) {
    AddSubstitution(V, VE, Direct);
  }

  // If we replace the Factor with a newly created expression we need to assign
  // it a version, killed factor's is fine i think.
  if (IsVersionUnset(VE)) VE->setVersion(FE->getVersion());

  KillFactor(FE, false);

  // We stiil need this link, because other instructions can reference this
  // Factor, not only its versions.
  AddSubstitution(FE, VE, Direct);
}

unsigned int SSAPRE::
GetRank(const Value *V) const {
  // Prefer undef to anything else
  if (isa<UndefValue>(V))
    return 0;

  if (isa<Constant>(V))
    return 1;
  else if (auto *A = dyn_cast<Argument>(V))
    return 2 + A->getArgNo();

  // Need to shift the instruction DFS by number of arguments + 3 to account for
  // the constant and argument ranking above.
  unsigned Result = InstrDFS.lookup(V);
  if (Result > 0)
    return 3 + NumFuncArgs + Result;

  // Unreachable or something else, just return a really large number.
  return ~0;
}

bool SSAPRE::
ShouldSwapOperands(const Value *A, const Value *B) const {
  // Because we only care about a total ordering, and don't rewrite expressions
  // in this order, we order by rank, which will give a strict weak ordering to
  // everything but constants, and then we order by pointer address.
  return std::make_pair(GetRank(A), A) > std::make_pair(GetRank(B), B);
}

bool SSAPRE::
FillInBasicExpressionInfo(Instruction &I, BasicExpression *E) {
  assert(E);

  bool AllConstant = true;

  // ??? This is a bit weird, do i actually need this?
  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    E->setType(GEP->getSourceElementType());
  } else {
    E->setType(I.getType());
  }

  E->setOpcode(I.getOpcode());

  for (auto &O : I.operands()) {
    if (auto *C = dyn_cast<Constant>(O)) {
      AllConstant &= true;

      // This is the first time we see this Constant
      if (!ValueToCOExp[C]) {
        auto CE = CreateConstantExpression(*C);
        AddConstant(CE, C);
      }
    } else {
      AllConstant = false;
    }
    E->addOperand(O);
  }

  return AllConstant;
}

std::pair<unsigned, unsigned> SSAPRE::
AssignDFSNumbers(BasicBlock *B, unsigned Start, InstrToOrderType *M) {
  unsigned End = Start;
  // if (MemoryAccess *MemPhi = MSSA->getMemoryAccess(B)) {
  //   InstrDFS[MemPhi] = End++;
  // }

  for (auto &I : *B) {
    if (M) (*M)[&I] = End++;
  }

  // All of the range functions taken half-open ranges (open on the end side).
  // So we do not subtract one from count, because at this point it is one
  // greater than the last instruction.
  return std::make_pair(Start, End);
}

Expression *SSAPRE::
CheckSimplificationResults(Expression *E, Instruction &I, Value *V) {
  if (!V) return nullptr;

  assert(isa<BasicExpression>(E) &&
         "We should always have had a basic expression here");

  if (auto C = dyn_cast<Constant>(V)) {
    ExpressionAllocator.Deallocate(E);
    return CreateConstantExpression(*C);

  } else if (isa<Argument>(V) || isa<GlobalVariable>(V)) {
    ExpressionAllocator.Deallocate(E);
    return CreateVariableExpression(*V);
  }

  return nullptr;
}

ConstantExpression *SSAPRE::
CreateConstantExpression(Constant &C) {
  auto *E = new (ExpressionAllocator) ConstantExpression(C);
  E->setOpcode(C.getValueID());
  E->setVersion(LastConstantVersion--);
  return E;
}

VariableExpression *SSAPRE::
CreateVariableExpression(Value &V) {
  auto *E = new (ExpressionAllocator) VariableExpression(V);
  E->setOpcode(V.getValueID());
  E->setVersion(LastVariableVersion--);
  return E;
}

Expression * SSAPRE::
CreateIgnoredExpression(Instruction &I) {
  auto *E = new (ExpressionAllocator) IgnoredExpression(&I);
  E->setOpcode(I.getOpcode());
  E->setVersion(LastIgnoredVersion--);
  return E;
}

Expression * SSAPRE::
CreateUnknownExpression(Instruction &I) {
  auto *E = new (ExpressionAllocator) UnknownExpression(&I);
  E->setOpcode(I.getOpcode());
  E->setVersion(LastIgnoredVersion--);
  return E;
}

Expression * SSAPRE::
CreateBasicExpression(Instruction &I) {
  auto *E = new (ExpressionAllocator) BasicExpression();

  bool AllConstant = FillInBasicExpressionInfo(I, E);

  if (I.isCommutative()) {
    // Ensure that commutative instructions that only differ by a permutation
    // of their operands get the same expression map by sorting the operand value
    // numbers.  Since all commutative instructions have two operands it is more
    // efficient to sort by hand rather than using, say, std::sort.
    assert(I.getNumOperands() == 2 && "Unsupported commutative instruction!");
    if (ShouldSwapOperands(E->getOperand(0), E->getOperand(1)))
      E->swapOperands(0, 1);
  }

  // Perform simplificaiton
  // We do not actually require simpler instructions but rather require them be
  // in a canonical form. Mainly we are interested in instructions that we
  // ignore, such as constants and variables.
  // TODO: Right now we only check to see if we get a constant result.
  // We may get a less than constant, but still better, result for
  // some operations.
  // IE
  //  add 0, x -> x
  //  and x, x -> x
  // We should handle this by simply rewriting the expression.
  if (auto *CI = dyn_cast<CmpInst>(&I)) {
    // Sort the operand value numbers so x<y and y>x get the same value
    // number.
    CmpInst::Predicate Predicate = CI->getPredicate();
    if (ShouldSwapOperands(E->getOperand(0), E->getOperand(1))) {
      E->swapOperands(0, 1);
      Predicate = CmpInst::getSwappedPredicate(Predicate);
    }
    E->setOpcode((CI->getOpcode() << 8) | Predicate);
    // TODO: 25% of our time is spent in SimplifyCmpInst with pointer operands
    assert(I.getOperand(0)->getType() == I.getOperand(1)->getType() &&
           "Wrong types on cmp instruction");
    assert((E->getOperand(0)->getType() == I.getOperand(0)->getType() &&
            E->getOperand(1)->getType() == I.getOperand(1)->getType()));
    Value *V = SimplifyCmpInst(Predicate, E->getOperand(0), E->getOperand(1),
                               *DL, TLI, DT, AC);
    if (auto *SE = CheckSimplificationResults(E, I, V))
      return SE;
  } else if (isa<SelectInst>(I)) {
    if (isa<Constant>(E->getOperand(0)) ||
        E->getOperand(0) == E->getOperand(1)) {
      assert(E->getOperand(1)->getType() == I.getOperand(1)->getType() &&
             E->getOperand(2)->getType() == I.getOperand(2)->getType());
      Value *V = SimplifySelectInst(E->getOperand(0), E->getOperand(1),
                                    E->getOperand(2), *DL, TLI, DT, AC);
      if (auto *SE = CheckSimplificationResults(E, I, V))
        return SE;
    }
  } else if (I.isBinaryOp()) {
    Value *V = SimplifyBinOp(E->getOpcode(), E->getOperand(0), E->getOperand(1),
                             *DL, TLI, DT, AC);
    if (auto *SE = CheckSimplificationResults(E, I, V))
      return SE;
  } else if (auto *BI = dyn_cast<BitCastInst>(&I)) {
    Value *V = SimplifyInstruction(BI, *DL, TLI, DT, AC);
    if (auto *SE = CheckSimplificationResults(E, I, V))
      return SE;
  } else if (isa<GetElementPtrInst>(I)) {
    Value *V = SimplifyGEPInst(E->getType(), E->getOperands(), *DL, TLI, DT, AC);
    if (auto *SE = CheckSimplificationResults(E, I, V))
      return SE;
  } else if (AllConstant) {
    // We don't bother trying to simplify unless all of the operands
    // were constant.
    // TODO: There are a lot of Simplify*'s we could call here, if we
    // wanted to.  The original motivating case for this code was a
    // zext i1 false to i8, which we don't have an interface to
    // simplify (IE there is no SimplifyZExt).

    SmallVector<Constant *, 8> C;
    for (Value *Arg : E->getOperands())
      C.emplace_back(cast<Constant>(Arg));

    if (Value *V = ConstantFoldInstOperands(&I, C, *DL, TLI))
      if (auto *SE = CheckSimplificationResults(E, I, V))
        return SE;
  }

  return E;
}

Expression *SSAPRE::
CreatePHIExpression(PHINode &I) {
  auto *E = new (ExpressionAllocator) PHIExpression(I.getParent());
  FillInBasicExpressionInfo(I, E);
  return E;
}

FactorExpression *SSAPRE::
CreateFactorExpression(const Expression &PE, const BasicBlock &B) {
  auto FE = new (ExpressionAllocator) FactorExpression(B);

  // The order we add these blocks is not important, since these blocks only
  // used to get proper Operands and Versions out of the Expression.
  for (auto S = pred_begin(&B), EE = pred_end(&B); S != EE; ++S) {
    auto PB = (BasicBlock *)*S;
    FE->addPred(PB, FE->getVExprNum());

    // Make sure this block is reachable and make bugpoint happy
    if (!ValueToExp[PB->getTerminator()]) FE->setVExpr(PB, GetBottom());
  }

  FE->setPExpr(&PE);
  ExprToPExpr[FE] = &PE;

  return FE;
}

Expression * SSAPRE::
CreateExpression(Instruction &I) {
  if (I.isTerminator()) {
    return CreateIgnoredExpression(I);
  }

  Expression * E = nullptr;
  switch (I.getOpcode()) {
  case Instruction::ExtractValue:
  case Instruction::InsertValue:
    // E = performSymbolicAggrValueEvaluation(I);
    break;
  case Instruction::PHI:
    E = CreatePHIExpression(cast<PHINode>(I));
    break;
  case Instruction::Call:
    // E = performSymbolicCallEvaluation(I);
    break;
  case Instruction::Store:
    // E = performSymbolicStoreEvaluation(I);
    break;
  case Instruction::Load:
    // E = performSymbolicLoadEvaluation(I);
    break;
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::BitCast:
    E = CreateBasicExpression(I);
    break;
  case Instruction::ICmp:
  case Instruction::FCmp:
    // E = performSymbolicCmpEvaluation(I);
    break;
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    E = CreateBasicExpression(I);
    break;
  case Instruction::Select:
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
  case Instruction::GetElementPtr:
    E = CreateBasicExpression(I);
    break;
  default:
    E = CreateUnknownExpression(I);
  }

  if (!E) E = CreateUnknownExpression(I);

  return E;
}

//===----------------------------------------------------------------------===//
// Solvers
//===----------------------------------------------------------------------===//

// PHI operands Prototype solver
namespace llvm {
namespace ssapre {
namespace phi_factoring {
typedef const Expression * Token_t;

struct PropDst_t {
  // Token know thus far
  Token_t TOK;
  // Destination that expects a new Token that is coalculate using TOK
  const PHINode * DST;

  PropDst_t() = delete;
  PropDst_t(Token_t TOK, const PHINode *DST)
    : TOK(TOK), DST(DST) {}
};

Token_t GetTopTok() { return (Expression *)0x704; }
Token_t GetBotTok() { return (Expression *)0x807; }
bool IsTopTok(Token_t T) { return T == GetTopTok(); }
bool IsBotTok(Token_t T) { return T == GetBotTok(); }
bool IsTopOrBottomTok(Token_t T) { return IsTopTok(T) || IsBotTok(T); }

// Rules:
//   T    ^ T    = T      Exp  ^ T    = Exp
//   Exp  ^ Exp  = Exp    ExpX ^ ExpY = F
//   Exp  ^ F    = F      F    ^ T    = F
//   F    ^ F    = F
Token_t
CalculateToken(Token_t A, Token_t B) {
  // T    ^ T    = T
  // Exp  ^ Exp  = Exp
  // F    ^ F    = F
  if (A == B) {
    return A;
  }

  // Exp  ^ T    = Exp
  if (IsTopTok(A) && !IsTopOrBottomTok(B)) {
    return B;
  } else if (!IsTopOrBottomTok(A) && IsTopTok(B)) {
    return A;
  }

  // Exp  ^ F    = F
  if (IsBotTok(A) && !IsTopOrBottomTok(B)) {
    return GetBotTok();
  } else if (!IsTopOrBottomTok(A) && IsBotTok(B)) {
    return GetBotTok();
  }

  // ExpX ^ ExpY = F
  // F    ^ T    = F
  return GetBotTok();
}

typedef DenseMap<const PHINode *, const FactorExpression *> PHIFactorMap_t;
typedef DenseMap<const PHINode *, Token_t> PHITokenMap_t;
typedef SmallVector<PropDst_t, 8> PropDstVector_t;
typedef DenseMap<const PHINode *, PropDstVector_t> SrcPropMap_t;
typedef DenseMap<const PHINode *, bool> PHIBoolMap_t;
typedef SmallVector<const PHINode *, 8> PHIVector_t;

enum TokenPropagationSolverType {
  // Accurate solver does gurantee that all factors it contains after the
  // executation have corret Token(PE) assigned to them
  TPST_Accurate,
  // Approximation takes somewhat more optimistic way using Top value for
  // constants and variables, this allows to match non-materialized factors to
  // PHIs. It is useful to prevent addition of superfluous Factors.
  TPST_Approximation
};

class TokenPropagationSolver {
  TokenPropagationSolverType TPST;
  SSAPRE &O;
  PHIFactorMap_t PHIFactorMap;
  PHITokenMap_t PHITokenMap;
  SrcPropMap_t SrcPropMap;
  PHIBoolMap_t SrcKillMap;
  PHIBoolMap_t FinishedMap;

public:
  TokenPropagationSolver() = delete;
  TokenPropagationSolver(TokenPropagationSolverType TPST, SSAPRE &O)
    : TPST(TPST), O(O) {}

  void
  CreateFactor(const PHINode *PHI, Token_t PE) {
    auto E = PHIFactorMap[PHI];
    assert(!E && "FE already exists");
    E = O.CreateFactorExpression(*PE, *PHI->getParent());
    PHIFactorMap[PHI] = E;
    SrcKillMap[PHI] = false;
    FinishedMap[PHI] = false;
  }

  bool
  HasTokenFor(const PHINode *PHI) {
    return PHITokenMap.count(PHI) != 0;
  }

  Token_t
  GetTokenFor(const PHINode *PHI) {
    if (HasFactorFor(PHI))
      return PHITokenMap[PHI];
    return GetBottom();
  }

  bool
  HasFactorFor(const PHINode *PHI) {
    return PHIFactorMap.count(PHI) != 0;
  }

  bool
  IsFinished(const PHINode *PHI) {
    assert(HasFactorFor(PHI));
    return FinishedMap[PHI];
  }

  const FactorExpression *
  GetFactorFor(const PHINode *PHI) {
    assert(HasFactorFor(PHI));
    return PHIFactorMap[PHI];
  }

  PHIFactorMap_t GetLiveFactors() { return PHIFactorMap; }

  void
  AddPropagations(Token_t T, const PHINode *S, PHIVector_t DL) {
    for (auto D : DL) { AddPropagation(T, S, D); }
  }

  void
  AddPropagation(Token_t T, const PHINode *S, const PHINode *D) {
    if (!HasFactorFor(S)) CreateFactor(S, T);
    if (!HasFactorFor(D)) CreateFactor(D, T);

    SrcKillMap[D] = IsBotTok(T);

    if (!SrcPropMap.count(S)) {
      SrcPropMap.insert({S, {{T, D}}});
    } else {
      SrcPropMap[S].push_back({T, D});
    }
  }

  void
  FinishPropagation(Token_t T, const PHINode *PHI) {
    assert(!SrcKillMap[PHI] && "The Factor is already killed");

    if (!HasFactorFor(PHI)) CreateFactor(PHI, T);
    PHITokenMap[PHI] = T;

    // Either Top or Bottom results in deletion of the Factor
    SrcKillMap[PHI] = IsTopOrBottomTok(T);

    FinishedMap[PHI] = true;

    if (!SrcPropMap.count(PHI)) return;

    // Recursively finish every propagation
    for (auto &PD : SrcPropMap[PHI]) {
      auto R = CalculateToken(T, PD.TOK);
      FinishPropagation(R, PD.DST);
    }
  }

  void
  Cleanup() {
    // Erase all killed Factors before returning the Map
    for (auto P : SrcKillMap) {
      if (!P.getSecond()) continue;

      O.ExpressionAllocator.Deallocate(P.getFirst());
      auto PHI = P.getFirst();
      PHIFactorMap.erase(PHI);
      PHITokenMap.erase(PHI);
    }
  }

  void
  Solve() {
    // Top-Down walk over join blocks set and try to calculate current PHIs PE.
    // If it happens that some operand of this PHI is produced by another PHI
    // that we yet to meet(back branch) we create a propagation record that
    // stores partial Token and these two PHIs as Source and Destination. If we
    // can calculate a Token immediately we "finish" current PHI and propagate
    // its Token to other PHIs that depend on it recursively. By the end of
    // this walk we will have a set of Factors that have either a legal Token
    // or a Bottom value as their Prototype Expression.
    for (auto B : O.JoinBlocks) {
      for (auto &I : *B) {

        // When we reach first non-phi instruction we stop
        if (&I == B->getFirstNonPHI()) break;

        auto PHI = dyn_cast<PHINode>(&I);
        if (!PHI) continue;

        auto PHIVE = O.ValueToExp[PHI];

        // Token is a meet of all the PHI's operands. We optimistically set it
        // initially to Top
        Token_t TOK = GetTopTok();

        // Back Branch source
        const PHINode * BackBranch = nullptr;


        for (unsigned i = 0, l = PHI->getNumOperands(); i < l; ++i) {
          auto Op = PHI->getOperand(i);
          auto OVE = O.ValueToExp[Op];

          // Can happen after other optimization passes
          while (auto OPHI = dyn_cast<PHINode>(Op)) {
            if (OPHI->getNumOperands() != 1) break;
            Op = OPHI->getIncomingValue(0);
            OVE = O.ValueToExp[Op];
          }

          // Self-loop gives an optimistic Top value
          if (OVE == PHIVE) {
            TOK = GetTopTok();
            continue;
          }

          // Ignored expressions produce Bottom value right away
          if (IgnoredExpression::classof(OVE) ||
              UnknownExpression::classof(OVE)) {
            TOK = GetBotTok();
            break;
          }

          // A variable or a constant regarded as Bottom value
          if (O.IsVariableOrConstant(OVE)) {
            TOK = CalculateToken(TOK,
                TPST == TPST_Approximation ? GetTopTok() : GetBotTok());

            continue;
          }

          if (auto OPHI = dyn_cast<PHINode>(Op)) {

            if (!HasFactorFor(OPHI)) {
              TOK = CalculateToken(TOK, GetTopTok());

              // It is more like a sane precaution during development phase;
              // the solver can be changed to handle multiple back-branches,
              // but I have yet to encounter a block that has more than one and
              // thus this "feature" is not implemented
              assert(!BackBranch && "Must not be a second Back Branch");
              BackBranch = OPHI;

            } else {
              Token_t T = IsFinished(OPHI) ? GetTokenFor(OPHI) : GetTopTok();
              TOK = CalculateToken(TOK, T);
            }

            continue;

            // Otherwise we use whatever this VE is prototyped by
          } else {
            TOK = CalculateToken(TOK, O.ExprToPExpr[OVE]);

            continue;
          }
        }

        // This PHI has back branches and we are still not sure whether it is a
        // materialized Factor.
        if (BackBranch) {

          // Even with the back branch if the TOK is bottom it won't change and
          // we can finish its "propagation" right now
          if (IsBotTok(TOK)) {
            FinishPropagation(TOK, PHI);

          // Or we have either an Expression or Top value to propagate
          // upwards. We get/create Factors for current PHI and its cycle PHI
          // operands and link them appropriately.
          } else {
            AddPropagation(TOK, /* Source */ BackBranch, /* Destination */ PHI);
          }

        } else {
          FinishPropagation(TOK, PHI);
        }
      }
    }

    Cleanup();
  }
};
} // namespace phi_factoring
} // namespace ssapre
} // namespace llvm

//===----------------------------------------------------------------------===//
// Pass Implementation
//===----------------------------------------------------------------------===//

void SSAPRE::
Init(Function &F) {
  LastVariableVersion = VR_VariableLo;
  LastConstantVersion = VR_ConstantLo;
  LastIgnoredVersion  = VR_IgnoredLo;

  for (auto &A : F.args()) {
    auto VAExp = CreateVariableExpression(A);
    ExpToValue[VAExp] = &A;
    ValueToExp[&A] = VAExp;
    VAExpToValue[VAExp] = &A;
    ValueToVAExp[&A] = VAExp;
  }

  AddSubstitution(GetBottom(), GetBottom());

  // Each block starts its count from N hundred thousands, this will allow us
  // add instructions within wide DFS/SDFS range
  unsigned ICountGrowth = 100000;
  unsigned ICount = ICountGrowth;

  DenseMap<const DomTreeNode *, unsigned> RPOOrdering;
  unsigned Counter = 0;
  for (auto &B : *RPOT) {
    if (!B->getSinglePredecessor()) {
      JoinBlocks.push_back(B);
    }

    auto *Node = DT->getNode(B);
    assert(Node && "RPO and Dominator tree should have same reachability");

    // Assign each block RPO index
    RPOOrdering[Node] = ++Counter;

    // Collect all the expressions
    for (auto &I : *B) {
      // Create ProtoExpresison, this expression will not be versioned and used
      // to bind Versioned Expressions of the same kind/class.
      auto PE = CreateExpression(I);
      for (auto &P : PExprToInsts) {
        auto EP = P.getFirst();
        if (PE->equals(*EP))
          PE = (Expression *)EP;
      }

      if (!PE->getProto() && !IgnoreExpression(PE)) {
        PE->setProto(I.clone());
      }
      // This is the real versioned expression
      Expression *VE = CreateExpression(I);

      AddExpression(PE, VE, &I, B);

      if (!PExprToVersions.count(PE)) {
        PExprToVersions.insert({PE, DenseMap<int,ExpVector_t>()});
      }
    }
  }

  // Sort dominator tree children arrays into RPO.
  for (auto &B : *RPOT) {
    auto *Node = DT->getNode(B);
    if (Node->getChildren().size() > 1) {
      std::sort(Node->begin(), Node->end(),
                [&RPOOrdering](const DomTreeNode *A, const DomTreeNode *B) {
                  return RPOOrdering[A] < RPOOrdering[B];
                });
    }
  }

  // Assign each instruction a DFS order number. This will be the main order
  // we traverse DT in.
  auto DFI = df_begin(DT->getRootNode());
  for (auto DFE = df_end(DT->getRootNode()); DFI != DFE; ++DFI) {
    auto B = DFI->getBlock();
    auto BlockRange = AssignDFSNumbers(B, ICount, &InstrDFS);
    ICount += BlockRange.second - BlockRange.first + ICountGrowth;
  }

  // Now we need to create Reverse Sorted Dominator Tree, where siblings sorted
  // in the opposite to RPO order. This order will give us a clue, when during
  // the normal traversal(using loop, not recursion) we go up the tree. For
  // example:
  //
  //      CFG:   RPO(CFG):        DT:       DFS(DT):     SDFS(DT):
  //
  //       a        a              a            a            a
  //      / \        \           / | \        / | \        / | \
  //     b   c    b - c         b  d  c  \\  c  b  d  \\  d  b  c
  //      \ /      \               |     //       /   //   \
  //       d        d              e             e          e
  //       |        |
  //       e        e
  //
  //  RPO(CFG): { a, c, b, d, e } // normal cfg rpo
  //  DFS(DT):  { a, b, d, e, c } // before reorder
  //  DFS(DT):  { a, c, b, d, e } // after reorder
  //
  //  SDFS(DT): { a, d, e, b, c } // after reverse reorder
  //  SDFSO(DFS(DT),SDFS(DT)): { 1, 5, 4, 2, 3 }
  //                               <  >  >  <
  //
  // So this SDFSO which maps our RPOish DFS(DT) onto SDFS order gives us
  // points where we must backtrace our context(stack or whatever we keep
  // updated). These are the places where the next SDFSO is less than the
  // previous one. With the example above traversal stack will look like this:
  //
  // DFS: a - c - b - d - e
  //
  //  1  2  3  4  5
  // ---------------
  //  a  c  b  d  e
  //     a  a  a  d
  //              a
  //
  for (auto &B : *RPOT) {
    auto *Node = DT->getNode(B);
    if (Node->getChildren().size() > 1) {
      std::sort(Node->begin(), Node->end(),
                [&RPOOrdering](const DomTreeNode *A, const DomTreeNode *B) {
                  // NOTE here we are using the reversed operator
                  return RPOOrdering[A] > RPOOrdering[B];
                });
    }
  }

  // Calculate Instruction-to-SDFS map
  ICount = ICountGrowth;
  DFI = df_begin(DT->getRootNode());
  for (auto DFE = df_end(DT->getRootNode()); DFI != DFE; ++DFI) {
    auto B = DFI->getBlock();
    auto BlockRange = AssignDFSNumbers(B, ICount, &InstrSDFS);
    ICount += BlockRange.second - BlockRange.first + ICountGrowth;
  }

  // Return DT to RPO order
  for (auto &B : *RPOT) {
    auto *Node = DT->getNode(B);
    if (Node->getChildren().size() > 1) {
      std::sort(Node->begin(), Node->end(),
                [&RPOOrdering](const DomTreeNode *A, const DomTreeNode *B) {
                  // NOTE here we are using the reversed operator
                  return RPOOrdering[A] < RPOOrdering[B];
                });
    }
  }
}

void SSAPRE::
Fini() {
  JoinBlocks.clear();

  ExpToValue.clear();
  ValueToExp.clear();

  VAExpToValue.clear();
  ValueToVAExp.clear();

  COExpToValue.clear();
  ValueToCOExp.clear();

  InstrDFS.clear();
  InstrSDFS.clear();

  FactorToPHI.clear();
  PHIToFactor.clear();

  InstToVExpr.clear();
  VExprToInst.clear();
  ExprToPExpr.clear();
  PExprToVersions.clear();
  PExprToInsts.clear();
  PExprToBlocks.clear();
  PExprToVExprs.clear();

  BlockToFactors.clear();
  FactorToBlock.clear();

  FExprs.clear();

  Substitutions.clear();
  KillList.clear();

  ExpressionAllocator.Reset();
}

void SSAPRE::
FactorInsertionMaterialized() {
  using namespace phi_factoring;
  TokenPropagationSolver TokSolver(TPST_Accurate, *this);
  TokSolver.Solve();

  // Process proven-to-be materialized Factor/PHIs
  for (auto &P : TokSolver.GetLiveFactors()) {
    auto PHI = (PHINode *)P.getFirst();
    auto B = PHI->getParent();
    auto F = (FactorExpression *)P.getSecond();
    auto T = TokSolver.GetTokenFor(PHI);

    if (IgnoreExpression(T)) continue;

    // Set already known expression versions
    for (unsigned i = 0, l = PHI->getNumOperands(); i < l; ++i) {
      auto B = PHI->getIncomingBlock(i);
      auto O = PHI->getOperand(i);

      // This is a switch, it better have the same value along multiple edges
      // it reaches this PHI
      if (auto OO = F->getVExpr(B)) {
        if (OO != ValueToExp[O])
          llvm_unreachable("This is the switch case i was affraid of");
      }

      if (auto OPHI = dyn_cast<PHINode>(O)) {

        // If the PHI is a back-branched Factor
        if (TokSolver.HasFactorFor(OPHI)) {
          F->setVExpr(B, (Expression *)TokSolver.GetFactorFor(OPHI));

        // Or maybe this PHI was already processed
        } else if (auto FE = PHIToFactor[OPHI]){
          F->setVExpr(B, (Expression *)FE);

        // If none above we just use PHIExpression
        } else {
          F->setVExpr(B, ValueToExp[O]);
        }

      } else {
        F->setVExpr(B, ValueToExp[O]);
      }
    }

    AddFactor(F, T, B);
    MaterializeFactor(F, (PHINode *)PHI);
  }
}

void SSAPRE::
FactorInsertionRegular() {
  // Insert Factors for every PE
  // Factors are inserted in two cases:
  //   - for each block in expressions IDF
  //   - for each phi of expression operand, which indicates expression
  //     alteration(TODO, requires operand versioning)
  for (auto &P : PExprToInsts) {
    auto &PE = P.getFirst();

    // Do not Factor PHIs, obviously
    if (IgnoreExpression(PE) || PHIExpression::classof(PE)) continue;

    // Each Expression occurrence's DF requires us to insert a Factor function,
    // which is much like PHI function but for expressions.
    SmallVector<BasicBlock *, 32> IDF;
    ForwardIDFCalculator IDFs(*DT);
    IDFs.setDefiningBlocks(PExprToBlocks[PE]);
    // IDFs.setLiveInBlocks(BlocksWithDeadTerminators);
    IDFs.calculate(IDF);

    for (const auto &B : IDF) {

      // True if a Factor for this Expression with exactly the same arguments
      // exists. There are two possibilities for arguments equality, there
      // either none which means it wasn't versioned yet, or there are
      // versions(or rather expression definitions) which means they were
      // spawned out of PHIs. We are concern with the first case for now.
      bool FactorExists = false;
      for (auto F : BlockToFactors[B]) {
        if (F->getPExpr() == PE) {
          if (F->getIsMaterialized()) {
            // TODO Is there a way not to do the rename.cleanup and reject
            // TODO factor insertion here. The reason why this requires a
            // TODO separate pass is that we do not know the actual operands
            // TODO before we run rename
          } else {
            FactorExists = true;
            break;
          }
        }
      }

      if (!FactorExists) {
        auto F = CreateFactorExpression(*PE, *B);
        AddFactor(F, PE, B);
      }
    }
  }
}

void SSAPRE::
FactorInsertion() {
  FactorInsertionMaterialized();
  DEBUG(PrintDebug("STEP 1: F-Insertion.Materialized"));

  FactorInsertionRegular();
  DEBUG(PrintDebug("STEP 1: F-Insertion.Regular"));
}

void SSAPRE::
RenamePass() {
  // We assign SSA versions to each of 3 kinds of expressions:
  //   - Real expression
  //   - Factor expression
  //   - Factor operands, these generally versioned as Bottom

  // The counters are used to number expression versions during DFS walk.
  // Before the renaming phase each instruction(that we do not ignore) is of a
  // proto type(PExpr), after this walk every expression is assign its own
  // version and it becomes a versioned(or instantiated) expression(VExpr).
  DenseMap<const Expression *, int> PExprToCounter;

  // Each PExpr is mapped to a stack of VExpr that grow and shrink during DFS
  // walk.
  PExprToVExprStack_t PExprToVExprStack;

  // Path we walk during DFS
  BBVector_t Path;

  // Init the stacks and counters
  for (auto &P : PExprToInsts) {
    auto &PE = P.getFirst();
    if (IgnoreExpression(PE)) continue;

    PExprToCounter.insert({PE, 0});
    PExprToVExprStack.insert({PE, {}});
  }

  auto DFI = df_begin(DT->getRootNode());
  for (auto DFE = df_end(DT->getRootNode()); DFI != DFE; ++DFI) {
    auto B = DFI->getBlock();
    // Since factors live outside basic blocks we set theirs DFS as the first
    // instruction's in the block
    auto FSDFS = InstrSDFS[&B->front()];

    // Backtrack the path if necessary
    while (!Path.empty() && InstrSDFS[&Path.back()->front()] > FSDFS)
      Path.pop_back();

    Path.push_back(B);

    // Set PHI versions first, since factors regarded as occurring at the end
    // of the predecessor blocks and PHIs go strictly before Factors
    // NOTE Currently there is no need to version non-factored PHIs, since the
    // only use for them would be to define an expression's operand, but without
    // phi-ud graph this is useless
    // NOTE resurect this when phi-ud is ready
    // for (auto &I : *B) {
    //   if (IsFactoredPHI(&I)) continue;
    //   if (&I == B->getFirstNonPHI()) break;
    //   auto &VE = InstToVExpr[&I];
    //   auto &PE = ExprToPExpr[VE];
    //   VE->setVersion(PExprToCounter[PE]++);
    // }

    // NOTE We want to stack MFactors specifically after the normal ones so the
    // NOTE expressions will assume their versions

    // First Process non-materialized Factors
    for (auto FE : BlockToFactors[B]) {
      if (FE->getIsMaterialized()) continue;
      auto PE = FE->getPExpr();
      FE->setVersion(PExprToCounter[PE]++);
      PExprToVExprStack[PE].push({FSDFS, FE});
    }

    // Then materialized ones
    for (auto FE : BlockToFactors[B]) {
      if (!FE->getIsMaterialized()) continue;
      auto PE = FE->getPExpr();
      FE->setVersion(PExprToCounter[PE]++);
      PExprToVExprStack[PE].push({FSDFS, FE});
    }

    // And the rest of the instructions
    for (auto &I : *B) {
      // Skip already passed PHIs
      if (PHINode::classof(&I)) continue;

      auto &VE = InstToVExpr[&I];
      auto &PE = ExprToPExpr[VE];
      auto SDFS = InstrSDFS[&I];

      // Backtrace every stacks if we jumped up the tree
      for (auto &P : PExprToVExprStack) {
        auto &VEStack = P.getSecond();
        while (!VEStack.empty() && VEStack.top().first > SDFS) {
          VEStack.pop();
        }
      }

      // Do nothing for ignored expressions
      if (IgnoreExpression(VE)) continue;

      auto &VEStack = PExprToVExprStack[PE];
      auto *VEStackTop = VEStack.empty() ? nullptr : VEStack.top().second;
      auto *VEStackTopF = VEStackTop
                            ? dyn_cast<FactorExpression>(VEStackTop)
                            : nullptr;

      // NOTE
      // We have to do opportunistic substitution additions, otherwise it is
      // impossible to move tightly coupled code fragments out of the loops.
      // The key idea lies in OperandDominate predicate, it always uses latest
      // substitution for the expressions' operands, e.g.
      //
      //                   -----------1-
      //                     %0 <-
      //                   -------------
      //            .-----------. |
      //           /       -----------2-
      //          /         x = F(x,⊥)
      //         /          y = F(y,⊥)
      //        /          -------------
      //       /             /       \
      //      /    -----------3-   -----------4-
      //     /      x = %0 + 1     -------------
      //    /       y = %x + 1
      //   /       -------------
      //  ._____________/
      //
      // Now look at expression x, all its operands dominate the Factor, thus
      // it assumes its version. The expression y on the other hand has two
      // possible ways to get its version. First, if we DO NOT add substitution
      // from x to its Factor we cannot prove that y's operand x dominates F(y)
      // (or in other words, the definiton of x happens before definiton of y
      // in block 2). The second possibility is that we add x -> F(x)
      // substitution, and the time we process y expression its x operand will
      // point at its factor F(x) and since factors dominate each other this
      // will allow as to prove that expression y in fact is the same as its
      // factor F(y) and it will assume its version; this will make F(y) a
      // cycled factor and later on this will allow x and y to be moved
      // together out of the loop.

      // Stack is empty
      if (!VEStackTop) {
        VE->setVersion(PExprToCounter[PE]++);
        VEStack.push({SDFS, VE});

      // Factor
      } else if (VEStackTopF) {

        // If every operands' definition dominates this Factor we are dealing
        // with the same expression and assign Factor's version
        if (OperandsDominate(VE, VEStackTopF)) {
          VE->setVersion(VEStackTop->getVersion());
          AddSubstitution(VE, VEStackTop);

        // Otherwise VE's operand(s) is(were) defined in this block and this
        // is indeed a new expression version
        } else {
          VE->setVersion(PExprToCounter[PE]++);
          VEStack.push({SDFS, VE});

          // STEP 3 Init: DownSafe
          // If the top of the stack contains a Factor expression and its
          // version is not used along this path we clear its DownSafe flag
          // because its result is not anticipated by any other expression:
          // ---------   ---------
          //       \       /
          //  ------------------
          //   %V = Factor(...)
          //
          //        ...
          //  ( N defs of %V)
          //  ( M uses of %V)
          //        ...
          //
          //   def an opd
          //   new V
          //  ------------------
          //  If M == 0 we clear the %V's DownSafe flag
          if (!FactorHasRealUseBefore(VEStackTopF, Path, VE)) {
            VEStackTopF->setDownSafe(false);
          }
        }

      // Real occurrence
      } else {
        // We need to campare all operands versions, if they don't match we are
        // dealing with a new expression
        // ??? Should we traverse the stack instead, in search of the similar VE
        // ??? Though tests show there is no effect anyway
        bool SameVersions = true;
        auto VEBE = dyn_cast<BasicExpression>(VE);
        auto VEStackTopBE = dyn_cast<BasicExpression>(VEStackTop);
        for (unsigned i = 0, l = VEBE->getNumOperands(); i < l; ++i) {
          if (ValueToExp[VEBE->getOperand(i)]->getVersion() !=
              ValueToExp[VEStackTopBE->getOperand(i)]->getVersion()) {
            SameVersions = false;
            break;
          }
        }

        if (SameVersions) {
          VE->setVersion(VEStackTop->getVersion());
          AddSubstitution(VE, VEStackTop);
        } else {
          VE->setVersion(PExprToCounter[PE]++);
          VEStack.push({SDFS, VE});
        }
      }

      PExprToVersions[PE][VE->getVersion()].push_back(VE);
    }

    // For a terminator we need to visit every cfg successor of this block
    // to update its Factor expressions
    auto T = B->getTerminator();
    for (auto S : T->successors()) {
      for (auto F : BlockToFactors[S]) {
        auto PE = F->getPExpr();
        auto &VEStack = PExprToVExprStack[PE];
        auto VEStackTop = VEStack.empty() ? nullptr : VEStack.top().second;
        auto VE = VEStack.empty() ? GetBottom() : VEStackTop;

        // Linked Factor's operands are already versioned and set
        if (F->getIsMaterialized()) {
          VE = F->getVExpr(B);
        } else {
          F->setVExpr(B, VE);
        }

        if (IsBottomOrVarOrConst(VE)) continue;

        // STEP 3 Init: HasRealUse
        bool HasRealUse = false;
        if (VEStackTop) {
          // To check Factor's usage we need to check usage of the Expressions
          // of the same version
          if (FactorExpression::classof(VEStackTop)) {
            HasRealUse = FactorHasRealUseBefore(
                           (FactorExpression *)VEStackTop,
                           Path,
                           InstToVExpr[T]);
          // If it is a real expression we check the usage directly
          } else if (BasicExpression::classof(VEStackTop)) {
            HasRealUse = HasRealUseBefore(VEStackTop, Path, InstToVExpr[T]);
          }
        }

        F->setHasRealUse(VE, HasRealUse);
      }
    }

    // STEP 3 Init: DownSafe
    // We set Factor's DownSafe to False if it is the last Expression's
    // occurence before program exit.
    if (T->getNumSuccessors() == 0) {
      for (auto &P : PExprToVExprStack) {
        auto &VEStack = P.getSecond();
        if (VEStack.empty()) continue;
        if (auto *F = dyn_cast<FactorExpression>(VEStack.top().second)) {
          if (!FactorHasRealUseBefore(F, Path, InstToVExpr[T]))
            F->setDownSafe(false);
        }
      }
    }
  }
}

void SSAPRE::
RenameCleaup() {
  SmallPtrSet<FactorExpression *, 32> FactorKillList;

  // We are interested only in comparing the non-materialized Factors and any
  // PHIs. The key idea here is that if a PHI is to have a Factor it would have
  // one already, and if all the operands of this comparison match we delete
  // the Factor for that same reason. This is a cleanup pass due to the
  // distinction between materialized and non-materialized Factor insertion.
  // See FactorInsertion routine for materialized Factors propagation.
  using namespace phi_factoring;
  TokenPropagationSolver TokSolver(TPST_Approximation, *this);
  TokSolver.Solve();
  for (auto B : JoinBlocks) {
    for (auto F : BlockToFactors[B]) {
      if (F->getIsMaterialized()) continue;

      for (auto &I : *B) {
        if (&I == B->getFirstNonPHI()) break;

        auto PHI = dyn_cast<PHINode>(&I);
        if (!PHI) continue;
        if (PHI->getNumOperands() != F->getVExprNum()) continue;

        auto PF = TokSolver.GetTokenFor(PHI);
        // The Solver can give Bottom for PHI in case its Factor was kill
        // during its pass
        if (!IsBottom(PF) && PF != F->getPExpr()) continue;

        bool Skip = false;
        bool Kill = true;
        for (unsigned i = 0, l = PHI->getNumOperands(); i < l; ++i) {
          auto PIB = PHI->getIncomingBlock(i);
          auto PV = PHI->getIncomingValueForBlock(PIB);
          auto FVE = F->getVExpr(PIB);

          // NOTE
          // Kinda a special case, while assigning versioned expressions to a
          // Factor we cannot infer that a variable or a constant is coming
          // from the predecessor and we assign it to ⊥, but a Linked Factor
          // will know for sure whether a constant/variable is involved.
          if ((IsVariableOrConstant(PV) || PHINode::classof(PV)) &&
              (IsBottom(FVE) || FactorExpression::classof(FVE)))
            continue;

          // Continuing from the previous check, if one the operands is a const
          // variable or bottom we skip further comparing because it is clearly
          // a mismatch
          if (IsVariableOrConstant(PV) || IsBottom(FVE)) {
            Skip = true;
            break;
          }

          // NOTE
          // Yet another special case, since we do not add same version on the
          // stack it is possible to have a Factor as an operand of itself,
          // this happens for back branches only. We treat such an operand as a
          // bottom and ignore it.
          if (FVE == F) continue;

          auto PIVE = ValueToExp[PV];
          if (PIVE && (FVE == PIVE || FVE->getVersion() == PIVE->getVersion()))
            continue;

          Kill = false;
          break;
        }

        if (Skip) continue;
        if (Kill) {
          FactorKillList.insert(F);
          break;
        }
      }
    }
  }

  for (auto F : FactorKillList) {
    if (auto P = F->getProto()) P->dropAllReferences();
    KillFactor(F);
    AddSubstitution(F, GetTop());
  }
}

void SSAPRE::
RenameInductivityPass() {
  // TODO this whole induction thing is way too simple
  // This maps induction expressions to its cycle head we could find so far. We
  // gonna iterate over their users and add them too, deleting any using
  // factors along the way.
  struct Induction_t
  {
    const BasicBlock * H;
    const Expression *PE;
    Induction_t() = delete;
    Induction_t(const BasicBlock *H, const Expression *PE)
      : H(H), PE(PE) {}
  };

  SmallVector<Induction_t, 8> Inductions;
  SmallPtrSet<FactorExpression *, 32> FactorKillList;


  // Determine cyclic Factors of whats left
  for (auto F : FExprs) {

    if (FactorKillList.count(F)) continue;
    for (auto VE : F->getVExprs()) {

      // Factors with related induction operands are useless, we cannot move
      // them or change, so just kill'em.
      // N.B. This collects the intial induction expression sets and related
      // header blocks, the following computation will not add new header
      // blocks but will find more induction expressions
      if (IsInductionExpression(F, VE)) {
        auto H = FactorToBlock[F];
        auto PE = ExprToPExpr[VE];
        Inductions.push_back({H, PE});
        FactorKillList.insert(F);
        AddSubstitution(VE, VE);

        // Find all the Factors within the loop that share the same PE
        auto HDFS = InstrDFS[&H->front()];
        auto IDFS = InstrDFS[VExprToInst[VE]];
        for (auto IF : FExprs) {
          if (IF->getPExpr() != PE) continue;
          auto IFB = FactorToBlock[IF];

          // This checks whether this Factor is within the cycle by assurring
          // its containing block's dfs is between header block's and induction
          // instruction's
          auto DFS = InstrDFS[&IFB->front()];
          if (DFS < HDFS || DFS > IDFS) continue;

          FactorKillList.insert(IF);
        }
        break;
      }

      // This happens if the Factor is contained inside a cycle and there is
      // not change in the expression's operands along this cycle.
      if (F->getVersion() == VE->getVersion()) {
        F->setIsCycle(VE, true);
      }
    }
  }

  for (unsigned i = 0, l = Inductions.size(); i < l; ++i) {
    auto IM = Inductions[i];
    auto IH = IM.H;
    auto IPE = IM.PE;

    for (auto F : BlockToFactors[IH]) {
      auto FPE = F->getPExpr();
      if (FPE == IPE) continue;

      for (auto &FPO : FPE->getProto()->operands()) {
        auto FPOE = ValueToExp[FPO];
        if (IgnoreExpression(FPOE)) continue;
        auto FPOPE = ExprToPExpr[FPOE];
        if (FPOPE != IPE) continue;
        FactorKillList.insert(F);
        Inductions.push_back({IH, FPE}); l++;
        break;
      }
    }
  }

  // Remove all stuff related
  for (auto F : FactorKillList) {

    if (auto P = F->getProto()) P->dropAllReferences();

    auto PHI = FactorToPHI[F];
    auto REP = PHI ? InstToVExpr[PHI] : GetTop();
    KillFactor(F);
    AddSubstitution(F, REP, /* direct */ true, /* force */ true);
  }
}

void SSAPRE::
Rename() {
  RenamePass();
  DEBUG(PrintDebug("Rename.Pass"));
  RenameCleaup();
  DEBUG(PrintDebug("Rename.Cleanup"));
  RenameInductivityPass();
  DEBUG(PrintDebug("Rename.InductivityPass"));
}

bool SSAPRE::
IsInductionExpression(const Expression *E) {
  if (BasicExpression::classof(E)) {
    for (auto &I : VExprToInst[E]->operands()) {
      if (auto PHI = dyn_cast<PHINode>(I.get())) {
        if (auto F = PHIToFactor[PHI]) {
          if (F->hasVExpr(E))
            return true;
        }
      }
    }
  }
  return false;
}

bool SSAPRE::
IsInductionExpression(const FactorExpression *F, const Expression *E) {
  if (BasicExpression::classof(E)) {
    for (auto &I : VExprToInst[E]->operands()) {
      if (auto PHI = dyn_cast<PHINode>(I.get())) {
        if (auto FF = PHIToFactor[PHI]) {
          if (F == FF)
            return true;
        }
      }
    }
  }
  return false;
}

void SSAPRE::
ResetDownSafety(FactorExpression *FE, Expression *E) {
  if (FE->getHasRealUse(E) || !FactorExpression::classof(E)) {
    return;
  }

  auto F = (FactorExpression *)E;
  if (!F->getDownSafe())
    return;

  F->setDownSafe(false);
  for (auto VE : F->getVExprs()) {
    ResetDownSafety(F, VE);
  }
}

void SSAPRE::
DownSafety() {
  // Here we propagate DownSafety flag initialized during Step 2 up the Factor
  // graph for each expression
  for (auto F : FExprs) {
    if (F->getDownSafe()) continue;
    for (auto VE : F->getVExprs()) {
      ResetDownSafety(F, VE);
    }
  }
}

void SSAPRE::
ComputeCanBeAvail() {
  for (auto F : FExprs) {
    if (!F->getDownSafe() && F->getCanBeAvail()) {
      for (auto V : F->getVExprs()) {
        if (IsBottom(V)) {
          ResetCanBeAvail(F);
          break;
        }
      }
    }
  }
}

void SSAPRE::
ResetCanBeAvail(FactorExpression *G) {
  G->setCanBeAvail(false);
  for (auto F : FExprs) {
    if (F->hasVExpr(G) && !F->getHasRealUse(G)) {

      // If it happens to be a cycle clear the flag
      if (F->getIsCycle(G)) {
        F->setIsCycle(G, false);
      }

      F->replaceVExpr(G, GetBottom());

      if (!F->getDownSafe() && F->getCanBeAvail()) {
        ResetCanBeAvail(F);
      }
    }
  }
}

void SSAPRE::
ComputeLater() {
  for (auto F : FExprs) {
    F->setLater(F->getCanBeAvail());
  }
  for (auto F : FExprs) {
    if (F->getLater()) {
      for (auto VE : F->getVExprs()) {
        if ((F->getHasRealUse(VE) || F->getIsCycle(VE)) && !IsBottom(VE)) {
          ResetLater(F);
          break;
        }
      }
    }
  }
}

void SSAPRE::
ResetLater(FactorExpression *G) {
  G->setLater(false);
  for (auto F : FExprs) {
    if (F->hasVExpr(G) && F->getLater()) ResetLater(F);
  }
}

void SSAPRE::
WillBeAvail() {
  ComputeCanBeAvail();
  ComputeLater();
}

void SSAPRE::
Finalize() {
  DenseMap<const Expression *, DenseMap<int, Expression *>> AvailDef;

  // Init available definitons map
  for (auto &P : PExprToInsts) {
    AvailDef.insert({P.getFirst(), DenseMap<int,Expression *>()});
  }

  // NOTE Using DT walk here is not really necessary because this loop does not
  // NOTE touch any successors
  auto DFI = df_begin(DT->getRootNode());
  for (auto DFE = df_end(DT->getRootNode()); DFI != DFE; ++DFI) {
    auto B = DFI->getBlock();

    for (auto F : BlockToFactors[B]) {
      auto V = F->getVersion();
      if (F->getWillBeAvail() || F->getAnyCycles() || F->getIsMaterialized()) {
        auto PE = F->getPExpr();
        AvailDef[PE][V] = F;
      }
    }

    for (auto &I : *B) {
      auto VE = InstToVExpr[&I];
      auto PE = ExprToPExpr[VE];

      // Traverse operands and add Save count to theirs definitions
      for (auto &O : I.operands()) {
        if (auto &E = ValueToExp[O]) {
            E->addSave();
        }
      }

      // We ignore these definitions
      if (IgnoreExpression(VE)) continue;

      // Restore substitution after Rename. This is necessary because there might
      // be records that bind an expression with a non available in any way
      // factor. This does not (or at least should not) break anything achieved
      // in rename since cycled operands considered available.

      AddSubstitution(VE, VE);
      if (auto PHI = dyn_cast<PHINode>(&I)) {
        if (PHI->getNumOperands() == 1) {
          auto PHIO = PHI->getIncomingValue(0);
          auto PHIOVE = ValueToExp[PHIO];
          assert(PHIOVE);
          AddSubstitution(VE, PHIOVE, /* direct */ true, /* force */ true);
        }
      }

      auto V = VE->getVersion();
      auto &ADPE = AvailDef[PE];
      auto DEF = ADPE[V];

      // If there was no expression occurrence before, or it was an expression's
      // operand definition, or the previous expression does not strictly
      // dominate the current occurrence we update the record
      if (!DEF || IsBottomOrVarOrConst(DEF) || !NotStrictlyDominates(DEF, VE)) {
        ADPE[V] = VE;

        // Otherwise, it is the same expression of the same version, and we just
        // add the substitution
      } else {
        AddSubstitution(VE, DEF);
      }
    }
  }
}

bool SSAPRE::
FactorCleanup(FactorExpression * F) {
  // Quick walk over Factor operands to check if we really need to insert
  // it, it is possible that the operands are all the same.
  Expression * O = nullptr;
  bool Same = true;
  bool HRU = false;
  for (auto P : F->getVExprs()) {
    HRU |= F->getHasRealUse(P);
    auto PS = GetSubstitution(P);
    if (O && O != PS) {
      Same = false;
      break;
    }
    O = PS;
  }

  // If all the ops are the same just use it
  if (Same) {
    // If the Factor is materialized we need to delay its replacement till
    // the substitution step
    if (F->getIsMaterialized()) {
      AddSubstitution(F, O);
      return false;
    } else {
      ReplaceFactor(F, O, HRU);
      return true;
    }
  }

  // We need to check whether all the arguments still present, if we
  // encounter a bottom we cannot spawn this PHI.
  bool Killed = false;
  for (auto P : F->getPreds()) {
    auto VE = F->getVExpr(P);
    auto SE = GetSubstitution(VE);
    if (IsBottom(SE) || IsTop(SE)) {
      Killed = true;
      break;
    }

    // Save the substitution
    F->setVExpr(P, SE);
  }

  if (Killed) {
    ReplaceFactor(F, GetTop());
    return true;
  }

  if (!F->getDownSafe() && !F->getIsMaterialized()) {
    ReplaceFactor(F, GetBottom());
    return true;
  }

  if (!F->getWillBeAvail() && !F->getIsMaterialized()) {
    // This forces all the expressions that point to this Factor point to
    // the previous expression or themselves.
    ReplaceFactor(F, GetTop());
    return true;
  }
  return false;
}

bool SSAPRE::
FactorGraphWalkBottomUp() {
  bool Changed = false;

  // bottom-up walk
  for (auto BS = JoinBlocks.rbegin(), BE = JoinBlocks.rend(); BS != BE; ++BS) {
    auto B = (BasicBlock *)*BS;
    auto List = BlockToFactors[B];
    for (auto FE : List) {
      auto PE = (Expression *)FE->getPExpr();

      if (FE->getAnyCycles()) {
        // There are two ways to deal with cycled factors, it all depends on
        // single non-cycled predecessor availability, if there is one we can
        // delete factor and replace all uses with it, otherwise the factor
        // stays or will be materialized later

        // N.B.
        // Cycled incoming values always match version with the Factor.
        // Technically cycles can exist even with versions that match not
        // Factor's but those are irrelevant to what we trying to achieve. Our
        // goal is to move non-changing expressions out of the cycle, the ones
        // that change inside cycle and therefore depend potentially on
        // induction variable(expressions) we cannot move before we move
        // expressions they depend on.

        // Cycled Expression
        SmallVector<Expression *, 8> CEV;

        // Non-Cycled incoming Expression
        Expression * VE = nullptr;

        // Non-Cycled incoming blocks
        BasicBlock * PB = nullptr;

        bool ShouldStay = false;
        bool CycledHRU = false;

        for (auto P : FE->getPreds()) {
          auto V = FE->getVExpr(P);

          if (FE->getIsCycle(V)) {
            CycledHRU |= FE->getHasRealUse(V);
            CEV.push_back(V);
            continue;
          }

          // Multiple non-cycled predecessors force this Factor to stay
          if (VE) ShouldStay = true;

          PB = P;
          VE = V;
        }

        // NOTE These peredicates force aggressive cycle hoisting
        if (ShouldStay ||

            IsVariableOrConstant(VE) || FactorExpression::classof(VE)) {

            // An incoming non-cycled expression that is not a real expression
            // forces this one to stay;
            // IsVariableOrConstant(VE) ||

            // N.B.
            // This is where profiling would be useful, we can prove whether
            // operands in these expressions dominate the factored phi and move
            // them up the cycle, thus precomputing the values, but here we act
            // conservatively and leave the expressions inside the cycle since
            // we do not know if we ever enter it

            // If there are more than one successors to the loop head we stay,
            // this is a conservative approach but with profiling this can
            // change
            // ((FactorExpression::classof(VE) || IsBottom(VE)) &&
            //  B->getTerminator()->getNumSuccessors() > 1)) {

          // By this time these cycled expression will point to the Factor, but
          // since it stays we these expressions must stay as well.
          for (auto CE : CEV)  AddSubstitution(CE, CE, /* direct */ true);
          continue; // no further processing
        }

        // Cycled sides is never used
        if (!CycledHRU && !FE->getDownSafe()) {
          Changed = ReplaceFactor(FE, GetBottom(), /* HRU */ false);
          continue; // no further processing
        }

        // TODO If there is no use of the expression inside the cycle move it
        // TODO to its successors
        auto T = PB->getTerminator();

        // Make sure the operands available at the predecessor block end
        if (!OperandsDominateStrictly(PE->getProto(), InstToVExpr[T])) continue;

        // At this point we only the only concern is whether the non-cycled
        // expression exist or not. Even if it is a variable or a const it is
        // not used due to the guard above
        bool HRU = FE->getHasRealUse(VE);
        if (IsBottomOrVarOrConst(VE)) {
          auto I = PE->getProto()->clone();
          VE = CreateExpression(*I);
          AddExpression(PE, VE, I, PB);
          auto T = PB->getTerminator();
          SetOrderBefore(I, T);
          SetAllOperandsSave(I);
          I->insertBefore(T);
          SSAPREInstrInserted++;
          HRU = false;
        }

        Changed = ReplaceFactor(FE, VE, HRU, /* direct */ true);
        continue; // no further processing

      } else if (FE->getDownSafe()) {
        // The Factor must be available and must not be cycled since those are
        // processed differently, and must not be materialized because those
        // already have their operands set
        if (FE->getWillBeAvail() && !FE->getIsMaterialized()) {
          auto PE = (Expression *)FE->getPExpr();
          for (auto BB : FE->getPreds()) {
            auto O = FE->getVExpr(BB);

            // Satisfies insert if either:
            if (
                // Version(O) is ⊥
                IsBottom(O) ||

                // HRU(O) is False and O is Factor and WBA(O) is False
                (!FE->getHasRealUse(O) && FactorExpression::classof(O) &&
                 !dyn_cast<FactorExpression>(O)->getWillBeAvail())) {

              auto PR = PE->getProto();
              if (!OperandsDominate(PR, FE)) break;

              auto I = PR->clone();
              auto VE = CreateExpression(*I);
              FE->setVExpr(BB, VE);
              AddExpression(PE, VE, I, BB);

              auto T = BB->getTerminator();
              SetOrderBefore(I, T);
              SetAllOperandsSave(I);
              I->insertBefore(T);
              SSAPREInstrInserted++;
            }
          }

          Changed = true;

          // If Mat and Later this Factor is useless and we replace it with a
          // real computation
        } else if (FE->getIsMaterialized() && FE->getLater() &&
            // Make sure this new instruction's operands will dominate this PHI
            OperandsDominate(PE->getProto(), FE)) {

          auto I = PE->getProto()->clone();
          auto VE = CreateExpression(*I);
          AddExpression(PE, VE, I, B);
          auto T = B->getFirstNonPHI();
          SetOrderBefore(I, T);
          SetAllOperandsSave(I);
          I->insertBefore((Instruction *)T);
          SSAPREInstrInserted++;

          ReplaceFactor(FE, VE, /* HRU */ false);
          Changed = true;
        }
      }

      FactorCleanup(FE);
    }
  }

  return Changed;
}

bool SSAPRE::
FactorGraphWalkTopBottom() {
  bool Changed = false;

  // top-down walk
  for (auto B : JoinBlocks) {
    auto List = BlockToFactors[B];
    for (auto F : List) {
      if (!F->getIsMaterialized())
        FactorCleanup(F);
    }
  }

  return Changed;
}

bool SSAPRE::
PHIInsertion() {
  bool Changed = false;

  // So we don't have to worry about order and back branches
  typedef std::pair<PHINode *, BasicBlock *> PHIPatch;
  typedef SmallVector<PHIPatch, 8> PHIPatchList;
  DenseMap<FactorExpression *, PHIPatchList> PHIPatches;

  // top-down walk
  for (auto B : JoinBlocks) {
    for (auto F : BlockToFactors[B]) {

      // Nothing to do here
      if (F->getIsMaterialized()) continue;

      IRBuilder<> Builder((Instruction *)B->getFirstNonPHI());
      auto TY = F->getPExpr()->getProto()->getType();
      auto PHI = Builder.CreatePHI(TY, F->getTotalPredecessors());
      PHI->setName("ssapre_phi");

      SSAPREPHIInserted++;

      // Fill-in PHI operands
      for (auto P : F->getPreds()) {
        auto VE = F->getVExpr(P);

        // If the operand is still non-materialized Factor we create a patch
        // point
        auto FVE = dyn_cast<FactorExpression>(VE);
        auto REP = F->GetPredMult(P);
        if (FVE && !FVE->getIsMaterialized()) {
          if (!PHIPatches.count(FVE)) PHIPatches.insert({FVE, {}});
          while (REP--) PHIPatches[FVE].push_back({PHI, P});
        } else {
          auto I = VExprToInst[VE];
          while (REP--) PHI->addIncoming(I, P);
        }

        // Add Save for each operand, since this Factor is live now
        VE->addSave();
      }

      // If there is a patch point awaiting this PHI
      if (PHIPatches.count(F)) {
        for (auto &PP : PHIPatches[F]) {
          auto PPHI = PP.first;
          auto PB = PP.second;
          PPHI->addIncoming(PHI, PB);
        }
      }

      // Make Factor Expression point to a real PHI
      MaterializeFactor(F, PHI);
      Changed = true;
    }
  }

  return Changed;
}

bool SSAPRE::
ApplySubstitutions() {
  bool Changed = false;

  for (auto P : VExprToInst) {
    if (!P.getFirst() || !P.getSecond()) {
      // llvm_unreachable("Why is this happening?");
      continue;
    }
    auto VE = (Expression *)P.getFirst();

    // This is a simplification result replacing the real instruction
    if (IsVariableOrConstant(VE)) {
      Value * T = nullptr;
      if (auto C = dyn_cast<ConstantExpression>(VE)) {
        T = &C->getConstant();
      } else if (auto V = dyn_cast<VariableExpression>(VE)) {
        T = &V->getValue();
      } else {
        llvm_unreachable("...");
      }

      auto VI = VExprToInst[VE];
      VI->replaceAllUsesWith(T);
      SSAPREInstrSubstituted++;
      KillList.push_back(VI);
      continue;
    }

    if (IsBottom(VE)) continue;
    if (IgnoreExpression(VE)) continue;
    // if (IsToBeKilled(VE)) continue;

    auto VI = VExprToInst[VE];
    auto SE = GetSubstitution(VE);

    // Top value forces this instruction to stay as is if there are uses
    if (IsTop(SE)) {
      // No uses? GTFO
      if (!VI->getNumUses()) KillList.push_back(VI);
      continue;
    }

    if (IsBottom(SE) || VE == SE) {
      // Standard case, instruction is not used at all and is not replaced by
      // anything. The only way for instruction to be substituted with a bottom
      // is when its Factor is deleted because of uselessness
      if (!FactorExpression::classof(VE) && !VE->getSave()) {
        assert(AllUsersKilled(VI));
        KillList.push_back(VI);
      }
      continue;
    }

    assert(!IsToBeKilled(SE));

    auto SI = (Instruction*)GetSubstituteValue(SE);
    assert(VI != SI && "Something went wrong");

    // Clear Save count of the original instruction
    VE->clrSave();

    // Check if this instruction is used at all.
    int RealUses = 0;
    for (auto U : VI->users()) {
      auto UI = (Instruction *)U;
      if (UI->getParent()) {
        RealUses++;
      }
    }

    // If this instruction does not have real use we subtract one Save from its
    // DIRECT sub
    if (RealUses == 0) {
      auto DS = GetSubstitution(VE, true);
      DS->remSave();
      if (!DS->getSave() && !IsToBeKilled(DS)) {
        KillList.push_back(VExprToInst[DS]);
      }
    }

    SE->addSave(RealUses);
    VI->replaceAllUsesWith(SI);
    SSAPREInstrSubstituted++;

    KillList.push_back(VI);

    Changed = true;
  }

  return Changed;
}

bool SSAPRE::
KillEmAll() {
  bool Changed = false;

  // Kill'em all
  // Before return we want to calculate effects of instruction deletion on the
  // other instructions. For example if we delete the last user of a value and
  // the instruction that produces this value does not have any side effects we
  // can delete it, and so on.
  for (unsigned i = 0, l = KillList.size(); i < l; ++i) {
    auto I = KillList[i];

    assert(AllUsersKilled(I) && "Should not be used by live instructions");

    // Decrease usage count of the instruction's operands
    for (auto &O : I->operands()) {
      if (auto &OE = ValueToExp[O]) {
        if (IgnoreExpression(OE)) continue;
        OE->remSave();
        if (!OE->getSave()) {
          KillList.push_back(VExprToInst[OE]);
          l++;
        }
      }
    }

    // Just drop the references for now
    I->dropAllReferences();
  }

  // Clear Protos
  for (auto &P : PExprToInsts) {
    auto *Proto = P.getFirst()->getProto();
    if (Proto)
      Proto->dropAllReferences();
  }

  // Remove instructions completely
  while (!KillList.empty()) {
    auto K = KillList.pop_back_val();
    if (!K->getParent()) continue;
    K->eraseFromParent();
    if (PHINode::classof(K))
      SSAPREPHIKilled++;
    else
      SSAPREInstrKilled++;
    Changed = true;
  }

  return Changed;
}

bool SSAPRE::
CodeMotion() {
  bool Changed = false;

  auto PI = (PrintInfo)(PI_Default | PI_Kill);

  Changed |= FactorGraphWalkBottomUp();
  DEBUG(PrintDebug("CodeMotion.FactorGraphWalkBottomUp", PI));

  Changed |= FactorGraphWalkTopBottom();
  DEBUG(PrintDebug("CodeMotion.FactorGraphWalkTopBottom", PI));

  Changed |= PHIInsertion();
  DEBUG(PrintDebug("CodeMotion.PHIInsertion", PI));

  Changed |= ApplySubstitutions();
  DEBUG(PrintDebug("CodeMotion.ApplySubstitutions", PI));

  Changed |= KillEmAll();
  // DEBUG(PrintDebug("CodeMotion.KillEmAll"));

  return Changed;
}

void SSAPRE::
PrintDebugInstructions() {
  dbgs() << "\n-Program----------------------------------\n";

  for (auto &B : *RPOT) {
    for (auto &I : *B) {
      dbgs() << "\n" << InstrSDFS[&I];
      dbgs() << "\t" << InstrDFS[&I];
      dbgs() << "\t" << I;
    }
  }

  dbgs() << "\n-----------------------------------------\n";
}

void SSAPRE::
PrintDebugExpressions(bool PrintIgnored) {
  dbgs() << "\n-Expressions-----------------------------\n";

  for (auto &P : PExprToInsts) {
    auto &PE = P.getFirst();
    if (IgnoreExpression(PE)) continue;
    dbgs() << "\n";
    dbgs() << ExpressionTypeToString(PE->getExpressionType());
    dbgs() << " " << (void *)PE;
    for (auto VE : PExprToVExprs[PE]) {
      auto I = VExprToInst[VE];
      dbgs() << "\n\t\t\t\t\t\t\t\t";
      dbgs() << " (" << InstrDFS[I] << ")";
      dbgs() << " (" << I->getName() << ")";
      dbgs() << " (";
      auto P = I->getParent();
      if (P)
        P->printAsOperand(dbgs());
      else
        dbgs() << "dead";
      dbgs() << ") ";
      VE->printInternal(dbgs());
    }
  }

  if (PrintIgnored) {
    dbgs() << "--------\n";
    for (auto &P : PExprToInsts) {
      auto &PE = P.getFirst();
      if (!IgnoreExpression(PE)) continue;
      dbgs() << "\n";
      dbgs() << ExpressionTypeToString(PE->getExpressionType());
      dbgs() << " " << (void *)PE;
      for (auto VE : PExprToVExprs[PE]) {
        auto I = VExprToInst[VE];
        dbgs() << "\n\t\t\t\t\t\t\t\t";
        dbgs() << " (" << InstrDFS[I]<< ")";
        dbgs() << " (" << I->getName() << ")";
        auto P = I->getParent();
        if (P)
          P->printAsOperand(dbgs());
        else
          dbgs() << "dead";
        dbgs() << ") ";
        VE->dump();
      }
    }
  }

  dbgs() << "\n-----------------------------------------\n";
}

void SSAPRE::
PrintDebugFactors() {
  dbgs() << "\n-BlockToFactors--------------------------\n";

  for (auto &B : *RPOT) {
    auto BTF = BlockToFactors[B];
    if (!BTF.size()) continue;
    dbgs() << "\n(" << BTF.size() << ") ";
    B->printAsOperand(dbgs(), false);
    dbgs() << ":";
    for (const auto &F : BTF) {
      dbgs() << "\n";
      F->printInternal(dbgs());
    }
  }

  dbgs() << "\n-----------------------------------------\n";
}

void SSAPRE::
PrintDebugSubstitutions() {
  dbgs() << "\n-Substitutions---------------------------\n";

  bool UseSeparator = true;
  bool PrintHeader = true;
  for (auto &PP : Substitutions) {
    auto PE = PP.getFirst();
    auto &MA = PP.getSecond();

    if (!MA.size()) continue;

    for (auto P : MA) {
      auto VE = P.getFirst();
      auto VI = VExprToInst[VE];
      auto SE = P.getSecond();
      auto SI = VExprToInst[SE];

      if (!VE) continue;
      if (IsTop(VE) || IsBottom(VE)) continue;
      if (IgnoreExpression(VE)) continue;
      if (VI && !VI->getParent()) continue;

      if (PrintHeader) {
        dbgs() << "\nPE: " << (void *)PE;
        PrintHeader = false;
      }

      dbgs() << "\n";

      if (auto FE = dyn_cast<FactorExpression>(VE)) {
        if (FE->getIsMaterialized() && FactorToPHI[FE]->getParent()) {
          dbgs() << "(F)";
          FactorToPHI[FE]->print(dbgs());
        } else {
          dbgs() << "     Factor V: " << FE->getVersion()
                 << ", MAT: " << (FE->getIsMaterialized() ? "T" : "F")
                 << ", PE: " << FE->getPExpr();
        }
      } else if (VI) {
        dbgs() << "(I)";
        VI->print(dbgs());
      } else {
        llvm_unreachable("Must not be the case");
      }

      dbgs() << " -> ";
      if (VE == SE) {
        dbgs() << "-";
      } else if (!SE) {
        dbgs() << "null -- SHOULD NOT BE LIKE THAT";
      } else if (IsTop(SE)) {
        dbgs() << "⊤";
      } else if (IsBottom(SE)) {
        dbgs() << "⊥";
      } else if (IsVariableOrConstant(SE)) {
        ExpToValue[SE]->print(dbgs());
      } else if (auto FE = dyn_cast<FactorExpression>(SE)) {
        if (FE->getIsMaterialized() && FactorToPHI[FE]->getParent()) {
          dbgs() << "(F) ";
          FactorToPHI[FE]->print(dbgs());
        } else {
          dbgs() << "     Factor V: " << FE->getVersion()
                 << ", MAT: " << (FE->getIsMaterialized() ? "T" : "F")
                 << ", PE: " << FE->getPExpr();
        }
      } else if (!SI->getParent()) {
        dbgs() << "(deleted)";
      } else {
        SI->print(dbgs());
      }
    }

    if (UseSeparator && !PrintHeader) {
      PrintHeader = true;
      dbgs() << "\n";
    }
  }

  dbgs() << "\n-----------------------------------------\n";
}

void SSAPRE::
PrintDebugKillist() {
  dbgs() << "\n-KillList--------------------------------\n";

  for (auto &K : KillList) {
    dbgs() << "\n";
    if (K->getParent()) {
      K->print(dbgs());
    } else {
      dbgs() << "\n(removed)";
    }
  }

  dbgs() << "\n-----------------------------------------\n";
}

void SSAPRE::
PrintDebug(const std::string &Caption, PrintInfo PI) {
  dbgs() << "\n" << Caption;
  dbgs() << "\n------------------------------------------------------------\n";
  if (PI & PI_Inst) PrintDebugInstructions();
  if (PI & PI_Expr) PrintDebugExpressions();
  if (PI & PI_Fact) PrintDebugFactors();
  if (PI & PI_Subs) PrintDebugSubstitutions();
  if (PI & PI_Kill) PrintDebugKillist();
  dbgs() << "\n------------------------------------------------------------\n";
}

PreservedAnalyses SSAPRE::
runImpl(Function &F,
        AssumptionCache &_AC,
        TargetLibraryInfo &_TLI, DominatorTree &_DT) {
  DEBUG(dbgs() << "SSAPRE(" << this << ") running on " << F.getName());

  bool Changed = false;

  TLI = &_TLI;
  DL = &F.getParent()->getDataLayout();
  AC = &_AC;
  DT = &_DT;
  Func = &F;

  NumFuncArgs = F.arg_size();

  RPOT = new ReversePostOrderTraversal<Function *>(&F);

  DEBUG(F.dump());

  Init(F);

  FactorInsertion();

  Rename();

  DownSafety();
  DEBUG(PrintDebug("STEP 3: DownSafety"));

  WillBeAvail();
  DEBUG(PrintDebug("STEP 4: WillBeAvail"));

  Finalize();
  DEBUG(PrintDebug("STEP 5: Finalize"));

  Changed = CodeMotion();

  Fini();

  DEBUG(F.dump());

  if (!Changed)
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}

PreservedAnalyses SSAPRE::run(Function &F, AnalysisManager<Function> &AM) {
  return runImpl(F,
      AM.getResult<AssumptionAnalysis>(F),
      AM.getResult<TargetLibraryAnalysis>(F),
      AM.getResult<DominatorTreeAnalysis>(F));
}


//===----------------------------------------------------------------------===//
// Pass Legacy
//===----------------------------------------------------------------------===//

class llvm::ssapre::SSAPRELegacy : public FunctionPass {
  DominatorTree *DT;

public:
  static char ID; // Pass identification, replacement for typeid.
  SSAPRE Impl;
  SSAPRELegacy() : FunctionPass(ID) {
    initializeSSAPRELegacyPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override { return "SSAPRE"; }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;

    auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
    auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    auto PA = Impl.runImpl(F, AC, TLI, DT);
    return !PA.areAllPreserved();
  }

private:
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
  }
};

char SSAPRELegacy::ID = 0;

// createSSAPREPass - The public interface to this file.
FunctionPass *llvm::createSSAPREPass() { return new SSAPRELegacy(); }

INITIALIZE_PASS_BEGIN(SSAPRELegacy,
                      "ssapre",
                      "SSA Partial Redundancy Elimination",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(BreakCriticalEdges)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(SSAPRELegacy,
                    "ssapre",
                    "SSA Partial Redundancy Elimination",
                    false, false)
