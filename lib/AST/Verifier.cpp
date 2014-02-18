//===--- Verifier.cpp - AST Invariant Verification ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements a verifier of AST invariants.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "swift/AST/ArchetypeBuilder.h"
#include "swift/AST/AST.h"
#include "swift/AST/ASTWalker.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
using namespace swift;

namespace {

template<typename T>
struct ASTNodeBase {};

#define EXPR(ID, PARENT) \
    template<> \
    struct ASTNodeBase<ID ## Expr *> { \
      typedef PARENT BaseTy; \
    };
#define ABSTRACT_EXPR(ID, PARENT) EXPR(ID, PARENT)
#include "swift/AST/ExprNodes.def"

#define STMT(ID, PARENT) \
    template<> \
    struct ASTNodeBase<ID ## Stmt *> { \
      typedef PARENT BaseTy; \
    };
#include "swift/AST/StmtNodes.def"

#define DECL(ID, PARENT) \
    template<> \
    struct ASTNodeBase<ID ## Decl *> { \
      typedef PARENT BaseTy; \
    };
#define ABSTRACT_DECL(ID, PARENT) DECL(ID, PARENT)
#include "swift/AST/DeclNodes.def"

#define PATTERN(ID, PARENT) \
    template<> \
    struct ASTNodeBase<ID ## Pattern *> { \
      typedef PARENT BaseTy; \
    };
#include "swift/AST/PatternNodes.def"

  enum ShouldHalt { Continue, Halt };

  class Verifier : public ASTWalker {
    PointerUnion<Module *, SourceFile *> M;
    ASTContext &Ctx;
    llvm::raw_ostream &Out;
    const bool HadError;
    SmallVector<bool, 8> InImplicitBraceStmt;

    /// \brief The stack of functions we're visiting.
    SmallVector<DeclContext *, 4> Functions;

    /// \brief The stack of scopes we're visiting.
    using ScopeLike = llvm::PointerUnion<DeclContext *, BraceStmt *>;
    SmallVector<ScopeLike, 4> Scopes;

    /// \brief The set of opaque value expressions active at this point.
    llvm::DenseMap<OpaqueValueExpr *, unsigned> OpaqueValues;

    /// The set of opened existential archetypes that are currently
    /// active.
    llvm::DenseSet<ArchetypeType *> OpenedExistentialArchetypes;

    /// A key into ClosureDiscriminators is a combination of a
    /// ("canonicalized") local DeclContext* and a flag for whether to
    /// use the explicit closure sequence (false) or the implicit
    /// closure sequence (true).
    typedef llvm::PointerIntPair<DeclContext*,1,bool> ClosureDiscriminatorKey;
    llvm::DenseMap<ClosureDiscriminatorKey,
                   llvm::SmallBitVector> ClosureDiscriminators;
    DeclContext *CanonicalTopLevelContext = nullptr;

  public:
    Verifier(Module *M, DeclContext *DC)
      : M(M), Ctx(M->Ctx), Out(llvm::errs()), HadError(M->Ctx.hadError()) {
      Scopes.push_back(DC);
    }
    Verifier(SourceFile &SF, DeclContext *DC)
      : M(&SF), Ctx(SF.getASTContext()), Out(llvm::errs()),
        HadError(SF.getASTContext().hadError()) {
      Scopes.push_back(DC);
    }

    static Verifier forDecl(const Decl *D) {
      DeclContext *DC = D->getDeclContext();
      DeclContext *topDC = DC->getModuleScopeContext();
      if (auto SF = dyn_cast<SourceFile>(topDC))
        return Verifier(*SF, DC);
      return Verifier(topDC->getParentModule(), DC);
    }

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      switch (E->getKind()) {
#define DISPATCH(ID) return dispatchVisitPreExpr(static_cast<ID##Expr*>(E))
#define EXPR(ID, PARENT) \
      case ExprKind::ID: \
        DISPATCH(ID);
#define UNCHECKED_EXPR(ID, PARENT) \
      case ExprKind::ID: \
        assert((HadError || !M.is<SourceFile*>() || \
                M.get<SourceFile*>()->ASTStage < SourceFile::TypeChecked) && \
               #ID "in wrong phase");\
        DISPATCH(ID);
#include "swift/AST/ExprNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    Expr *walkToExprPost(Expr *E) override {
      switch (E->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Expr*>(E))
#define EXPR(ID, PARENT) \
      case ExprKind::ID: \
        DISPATCH(ID);
#define UNCHECKED_EXPR(ID, PARENT) \
      case ExprKind::ID: \
        assert((HadError || !M.is<SourceFile*>() || \
                M.get<SourceFile*>()->ASTStage < SourceFile::TypeChecked) && \
               #ID "in wrong phase");\
        DISPATCH(ID);
#include "swift/AST/ExprNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
      switch (S->getKind()) {
#define DISPATCH(ID) return dispatchVisitPreStmt(static_cast<ID##Stmt*>(S))
#define STMT(ID, PARENT) \
      case StmtKind::ID: \
        DISPATCH(ID);
#include "swift/AST/StmtNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    Stmt *walkToStmtPost(Stmt *S) override {
      switch (S->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Stmt*>(S))
#define STMT(ID, PARENT) \
      case StmtKind::ID: \
        DISPATCH(ID);
#include "swift/AST/StmtNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    std::pair<bool, Pattern*> walkToPatternPre(Pattern *P) override {
      switch (P->getKind()) {
#define DISPATCH(ID) \
        return dispatchVisitPrePattern(static_cast<ID##Pattern*>(P))
#define PATTERN(ID, PARENT) \
      case PatternKind::ID: \
        DISPATCH(ID);
#include "swift/AST/PatternNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    Pattern *walkToPatternPost(Pattern *P) {
      switch (P->getKind()) {
#define DISPATCH(ID) \
        return dispatchVisitPost(static_cast<ID##Pattern*>(P))
#define PATTERN(ID, PARENT) \
      case PatternKind::ID: \
        DISPATCH(ID);
#include "swift/AST/PatternNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    bool walkToDeclPre(Decl *D) override {
      switch (D->getKind()) {
#define DISPATCH(ID) return dispatchVisitPre(static_cast<ID##Decl*>(D))
#define DECL(ID, PARENT) \
      case DeclKind::ID: \
        DISPATCH(ID);
#include "swift/AST/DeclNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    bool walkToDeclPost(Decl *D) override {
      switch (D->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Decl*>(D))
#define DECL(ID, PARENT) \
      case DeclKind::ID: \
        DISPATCH(ID);
#include "swift/AST/DeclNodes.def"
#undef DISPATCH
      }

      llvm_unreachable("Unhandled declaratiom kind");
    }

  private:
    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> bool dispatchVisitPre(T node) {
      return shouldVerify(node);
    }

    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> std::pair<bool, Expr *> dispatchVisitPreExpr(T node) {
      return { shouldVerify(node), node };
    }

    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> std::pair<bool, Stmt *> dispatchVisitPreStmt(T node) {
      return { shouldVerify(node), node };
    }

    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T>
    std::pair<bool, Pattern *> dispatchVisitPrePattern(T node) {
      return { shouldVerify(node), node };
    }

    /// Helper template for dispatching post-visitation.
    template <class T> T dispatchVisitPost(T node) {
      // Verify source ranges if the AST node was parsed from source.
      SourceFile *SF = M.dyn_cast<SourceFile *>();
      if (SF) {
        // If we are inside an implicit BraceStmt, don't verify source
        // locations.  LLDB creates implicit BraceStmts which contain a mix of
        // generated/user-written code.
        if (InImplicitBraceStmt.empty() || !InImplicitBraceStmt.back())
          checkSourceRanges(node);
      }

      // Check that nodes marked invalid have the correct type.
      checkErrors(node);

      // Always verify the node as a parsed node.
      verifyParsed(node);

      // If we've bound names already, verify as a bound node.
      if (!SF || SF->ASTStage >= SourceFile::NameBound)
        verifyBound(node);

      // If we've checked types already, do some extra verification.
      if (!SF || SF->ASTStage >= SourceFile::TypeChecked) {
        verifyCheckedAlways(node);
        if (!HadError)
          verifyChecked(node);
      }

      // Clean up anything that we've placed into a stack to check.
      cleanup(node);

      // Always continue.
      return node;
    }

    // Default cases for whether we should verify within the given subtree.
    bool shouldVerify(Expr *E) { return true; }
    bool shouldVerify(Stmt *S) { return true; }
    bool shouldVerify(Pattern *S) { return true; }
    bool shouldVerify(Decl *S) { return true; }

    // Default cases for cleaning up as we exit a node.
    void cleanup(Expr *E) { }
    void cleanup(Stmt *S) { }
    void cleanup(Pattern *P) { }
    void cleanup(Decl *D) { }
    
    // Base cases for the various stages of verification.
    void verifyParsed(Expr *E) {}
    void verifyParsed(Stmt *S) {}
    void verifyParsed(Pattern *P) {}
    void verifyParsed(Decl *D) {
      if (!D->getDeclContext()) {
        Out << "every Decl should have a DeclContext";
        abort();
      }
    }
    template<typename T>
    void verifyParsedBase(T ASTNode) {
      verifyParsed(cast<typename ASTNodeBase<T>::BaseTy>(ASTNode));
    }

    void verifyBound(Expr *E) {}
    void verifyBound(Stmt *S) {}
    void verifyBound(Pattern *P) {}
    void verifyBound(Decl *D) {}

    /// @{
    /// These verification functions are always run on type checked ASTs
    /// (even if there were errors).
    void verifyCheckedAlways(Expr *E) {}
    void verifyCheckedAlways(Stmt *S) {}
    void verifyCheckedAlways(Pattern *P) {}
    void verifyCheckedAlways(Decl *D) {}

    template<typename T>
    void verifyCheckedAlwaysBase(T ASTNode) {
      verifyCheckedAlways(cast<typename ASTNodeBase<T>::BaseTy>(ASTNode));
    }
    /// @}

    /// @{
    /// These verification functions are run on type checked ASTs if there were
    /// no errors.
    void verifyChecked(Expr *E) {
      bool failed = E->getType().findIf([&](Type type) -> bool {
          if (auto archetypeTy = type->getAs<ArchetypeType>()) {
            if (archetypeTy->getOpenedExistentialType()) {
              if (OpenedExistentialArchetypes.count(archetypeTy) == 0) {
                Out << "Found opened existential archetype "
                    << archetypeTy->getString()
                    << " outside enclosing OpenExistentialExpr\n";
                
                E->dump(Out);
                return true;
              }
            }
          }
          return false;
        });
      if (failed) assert(false);
    }

    void verifyChecked(Stmt *S) {}
    void verifyChecked(Pattern *P) {}
    void verifyChecked(Decl *D) {}

    template<typename T>
    void verifyCheckedBase(T ASTNode) {
      verifyChecked(cast<typename ASTNodeBase<T>::BaseTy>(ASTNode));
    }
    /// @}

    // Specialized verifiers.

    template <class T> void pushScope(T *scope) {
      Scopes.push_back(scope);
    }
    void popScope(DeclContext *scope) {
      assert(Scopes.back().get<DeclContext*>() == scope);
      Scopes.pop_back();
    }
    void popScope(BraceStmt *scope) {
      assert(Scopes.back().get<BraceStmt*>() == scope);
      Scopes.pop_back();
    }

    void pushFunction(DeclContext *functionScope) {
      pushScope(functionScope);
      Functions.push_back(functionScope);
    }
    void popFunction(DeclContext *functionScope) {
      assert(Functions.back() == functionScope);
      Functions.pop_back();
      popScope(functionScope);
    }

#define FUNCTION_LIKE(NODE)                                     \
    bool shouldVerify(NODE *fn) {                               \
      pushFunction(fn);                                         \
      return shouldVerify(cast<ASTNodeBase<NODE*>::BaseTy>(fn));\
    }                                                           \
    void cleanup(NODE *fn) {                                    \
      popFunction(fn);                                          \
    }
#define SCOPE_LIKE(NODE)                                        \
    bool shouldVerify(NODE *fn) {                               \
      pushScope(fn);                                            \
      if (fn->hasLazyMembers())                                 \
        return false;                                           \
      return shouldVerify(cast<ASTNodeBase<NODE*>::BaseTy>(fn));\
    }                                                           \
    void cleanup(NODE *fn) {                                    \
      popScope(fn);                                             \
    }

    FUNCTION_LIKE(AbstractClosureExpr)
    FUNCTION_LIKE(ConstructorDecl)
    FUNCTION_LIKE(DestructorDecl)
    FUNCTION_LIKE(FuncDecl)
    SCOPE_LIKE(NominalTypeDecl)
    SCOPE_LIKE(ExtensionDecl)

#undef SCOPE_LIKE
#undef FUNCTION_LIKE

    bool shouldVerify(BraceStmt *BS) {
      pushScope(BS);
      InImplicitBraceStmt.push_back(BS->isImplicit());
      return shouldVerify(cast<Stmt>(BS));
    }

    void cleanup(BraceStmt *BS) {
      InImplicitBraceStmt.pop_back();
      popScope(BS);
    }

    bool shouldVerify(OpenExistentialExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;

      OpaqueValues[expr->getOpaqueValue()] = 0;
      assert(OpenedExistentialArchetypes.count(expr->getOpenedArchetype())==0);
      OpenedExistentialArchetypes.insert(expr->getOpenedArchetype());
      return true;
    }

    void cleanup(OpenExistentialExpr *expr) {
      OpaqueValues.erase(expr->getOpaqueValue());
      assert(OpenedExistentialArchetypes.count(expr->getOpenedArchetype())==1);
      OpenedExistentialArchetypes.erase(expr->getOpenedArchetype());
    }

    /// Canonicalize the given DeclContext pointer, in terms of
    /// producing something that can be looked up in
    /// ClosureDiscriminators.
    DeclContext *getCanonicalDeclContext(DeclContext *DC) {
      // All we really need to do is use a single TopLevelCodeDecl.
      if (auto topLevel = dyn_cast<TopLevelCodeDecl>(DC)) {
        if (!CanonicalTopLevelContext)
          CanonicalTopLevelContext = topLevel;
        return CanonicalTopLevelContext;
      }

      // TODO: check for uniqueness of initializer contexts?

      return DC;
    }

    /// Return the appropriate discriminator set for a closure expression.
    llvm::SmallBitVector &getClosureDiscriminators(AbstractClosureExpr *closure) {
      auto dc = getCanonicalDeclContext(closure->getParent());
      bool isAutoClosure = isa<AutoClosureExpr>(closure);
      return ClosureDiscriminators[ClosureDiscriminatorKey(dc, isAutoClosure)];
    }

    void verifyCheckedAlways(ValueDecl *D) {
      if (D->hasType() && D->getType()->hasTypeVariable()) {
        Out << "a type variable escaped the type checker";
        D->dump(Out);
        abort();
      }
      if (auto Overridden = D->getOverriddenDecl()) {
        if (D->getDeclContext() == Overridden->getDeclContext()) {
          Out << "can not override a decl in the same DeclContext";
          D->dump(Out);
          Overridden->dump(Out);
          abort();
        }
      }
      if (D->conformsToProtocolRequirement()) {
        if (D->getConformances().empty()) {
          Out << "conforms bit set but no conformances found\n";
          D->dump(Out);
          abort();
        }
      }
      verifyCheckedAlwaysBase(D);
    }

    void verifyChecked(ReturnStmt *S) {
      auto func = Functions.back();
      Type resultType;
      if (FuncDecl *FD = dyn_cast<FuncDecl>(func)) {
        resultType = FD->getResultType();
      } else if (auto closure = dyn_cast<AbstractClosureExpr>(func)) {
        resultType = closure->getResultType();
      } else {
        resultType = TupleType::getEmpty(Ctx);
      }
      
      if (S->hasResult()) {
        auto result = S->getResult();
        auto returnType = result->getType();
        // Make sure that the return has the same type as the function.
        checkSameType(resultType, returnType, "return type");
      } else {
        // Make sure that the function has a Void result type.
        checkSameType(resultType, TupleType::getEmpty(Ctx), "return type");
      }

      verifyCheckedBase(S);
    }

    void checkCondition(StmtCondition C) {
      if (auto E = C.dyn_cast<Expr*>()) {
        checkSameType(E->getType(), BuiltinIntegerType::get(1, Ctx),
                      "condition type");
        return;
      }
      if (auto CB = C.dyn_cast<PatternBindingDecl*>()) {
        if (!CB->isConditional()) {
          Out << "condition binding is not conditional\n";
          CB->print(Out);
          abort();
        }
        if (!CB->getInit()) {
          Out << "conditional binding does not have initializer\n";
          CB->print(Out);
          abort();
        }
        auto initOptionalType = CB->getInit()->getType();
        auto initType = initOptionalType->getAnyOptionalObjectType();
        if (!initType) {
          Out << "conditional binding is not of optional type\n";
          CB->print(Out);
          abort();
        }
        checkSameType(CB->getPattern()->getType(), initType,
                      "conditional binding type");
      }
    }
    
    void verifyChecked(IfStmt *S) {
      checkCondition(S->getCond());
      verifyCheckedBase(S);
    }

    void verifyChecked(WhileStmt *S) {
      checkCondition(S->getCond());
      verifyCheckedBase(S);
    }

    Type checkAssignDest(Expr *Dest) {
      if (TupleExpr *TE = dyn_cast<TupleExpr>(Dest)) {
        SmallVector<TupleTypeElt, 4> lhsTupleTypes;
        for (unsigned i = 0; i != TE->getNumElements(); ++i) {
          Type SubType = checkAssignDest(TE->getElement(i));
          lhsTupleTypes.push_back(TupleTypeElt(SubType, TE->getElementName(i)));
        }
        return TupleType::get(lhsTupleTypes, Ctx);
      }
      return checkLValue(Dest->getType(), "LHS of assignment");
    }

    void verifyChecked(DeclRefExpr *E) {
      if (E->getType()->is<PolymorphicFunctionType>()) {
        Out << "unspecialized reference with polymorphic type "
          << E->getType().getString() << "\n";
        E->dump(Out);
        abort();
      }
      verifyCheckedBase(E);
    }

    void verifyChecked(AssignExpr *S) {
      Type lhsTy = checkAssignDest(S->getDest());
      checkSameType(lhsTy, S->getSrc()->getType(), "assignment operands");
      verifyCheckedBase(S);
    }

    void verifyChecked(AddressOfExpr *E) {
      Type srcObj = checkLValue(E->getSubExpr()->getType(),
                                "result of AddressOfExpr");
      auto DestTy = E->getType()->castTo<InOutType>()->getObjectType();
      
      checkSameType(DestTy, srcObj, "object types for AddressOfExpr");
      verifyCheckedBase(E);
    }

    void verifyParsed(AbstractClosureExpr *E) {
      Type Ty = E->getType();
      if (!Ty)
        return;
      if (Ty->is<ErrorType>())
        return;
      if (!Ty->is<FunctionType>()) {
        Out << "a closure should have a function type";
        E->print(Out);
        Out << "\n";
        abort();
      }
      verifyParsedBase(E);
    }

    void verifyChecked(AbstractClosureExpr *E) {
      assert(Scopes.back().get<DeclContext*>() == E);
      assert(E->getParent()->isLocalContext() &&
             "closure expression was not in local context!");

      // Check that the discriminator is unique in its context.
      auto &discriminatorSet = getClosureDiscriminators(E);
      unsigned discriminator = E->getDiscriminator();
      if (discriminator >= discriminatorSet.size()) {
        discriminatorSet.resize(discriminator+1);
        discriminatorSet.set(discriminator);
      } else if (discriminatorSet.test(discriminator)) {
        Out << "a closure must have a unique discriminator in its context\n";
        E->print(Out);
        Out << "\n";
        abort();
      } else {
        discriminatorSet.set(discriminator);
      }

      // If the enclosing scope is a DC directly, rather than a local scope,
      // then the closure should be parented by an Initializer.  Otherwise,
      // it should be parented by the innermost function.
      auto enclosingScope = Scopes[Scopes.size() - 2];
      auto enclosingDC = enclosingScope.dyn_cast<DeclContext*>();
      if (enclosingDC && !isa<AbstractClosureExpr>(enclosingDC)
          && !(isa<SourceFile>(enclosingDC)
               && cast<SourceFile>(enclosingDC)->Kind == SourceFileKind::REPL)){
        auto parentDC = E->getParent();
        if (!isa<Initializer>(parentDC)) {
          Out << "a closure in non-local context should be parented "
                 "by an initializer or REPL context";
          E->print(Out);
          Out << "\n";
          abort();
        } else if (parentDC->getParent() != enclosingDC) {
          Out << "closure in non-local context not grandparented by its "
                 "enclosing function";
          E->print(Out);
          Out << "\n";
          abort();
        }
      } else if (Functions.size() >= 2 &&
                 Functions[Functions.size() - 2] != E->getParent()) {
        Out << "closure in local context not parented by its "
               "enclosing function";
        E->print(Out);
        Out << "\n";
        abort();
      }

      if (E->getDiscriminator() == AbstractClosureExpr::InvalidDiscriminator) {
        Out << "a closure expression should have a valid discriminator\n";
        E->print(Out);
        Out << "\n";
        abort();
      }
    }

    void verifyChecked(MetatypeConversionExpr *E) {
      auto destTy = checkMetatypeType(E->getType(),
                                      "result of MetatypeConversionExpr");
      auto srcTy = checkMetatypeType(E->getSubExpr()->getType(),
                                     "source of MetatypeConversionExpr");

      if (destTy->isEqual(srcTy)) {
        Out << "trivial MetatypeConversionExpr:\n";
        E->print(Out);
        Out << "\n";
        abort();
      }

      checkTrivialSubtype(srcTy, destTy, "MetatypeConversionExpr");
      verifyCheckedBase(E);
    }

    void verifyChecked(DerivedToBaseExpr *E) {
      auto destTy = E->getType();
      auto srcTy = E->getSubExpr()->getType();
      if (destTy->isEqual(srcTy)) {
        Out << "trivial DerivedToBaseExpr:\n";
        E->print(Out);
        Out << "\n";
        abort();
      }

      if (!destTy->getClassOrBoundGenericClass() ||
          !(srcTy->getClassOrBoundGenericClass() ||
            srcTy->is<DynamicSelfType>())) {
        Out << "DerivedToBaseExpr does not involve class types:\n";
        E->print(Out);
        Out << "\n";
        abort();
      }

      checkTrivialSubtype(srcTy, destTy, "DerivedToBaseExpr");
      verifyCheckedBase(E);
    }

    void verifyChecked(TupleElementExpr *E) {
      Type resultType = E->getType();
      Type baseType = E->getBase()->getType();
      checkSameLValueness(baseType, resultType,
                          "base and result of TupleElementExpr");

      TupleType *tupleType = baseType->getAs<TupleType>();
      if (!tupleType) {
        Out << "base of TupleElementExpr does not have tuple type: ";
        E->getBase()->getType().print(Out);
        Out << "\n";
        abort();
      }

      if (E->getFieldNumber() >= tupleType->getFields().size()) {
        Out << "field index " << E->getFieldNumber()
            << " for TupleElementExpr is out of range [0,"
            << tupleType->getFields().size() << ")\n";
        abort();
      }

      checkSameType(resultType, tupleType->getElementType(E->getFieldNumber()),
                    "TupleElementExpr and the corresponding tuple element");
      verifyCheckedBase(E);
    }

    void verifyChecked(ApplyExpr *E) {
      FunctionType *FT = E->getFn()->getType()->getAs<FunctionType>();
      if (!FT) {
        Out << "callee of apply expression does not have function type:";
        E->getFn()->getType().print(Out);
        Out << "\n";
        abort();
      }
      CanType InputExprTy = E->getArg()->getType()->getCanonicalType();
      CanType ResultExprTy = E->getType()->getCanonicalType();
      if (ResultExprTy != FT->getResult()->getCanonicalType()) {
        Out << "result of ApplyExpr does not match result type of callee:";
        E->getType().print(Out);
        Out << " vs. ";
        FT->getResult()->print(Out);
        Out << "\n";
        abort();
      }
      if (InputExprTy != FT->getInput()->getCanonicalType()) {
        TupleType *TT = FT->getInput()->getAs<TupleType>();
        if (isa<SelfApplyExpr>(E)) {
          Type InputExprObjectTy;
          if (InputExprTy->hasReferenceSemantics() ||
              InputExprTy->is<MetatypeType>())
            InputExprObjectTy = InputExprTy;
          else
            InputExprObjectTy = checkLValue(InputExprTy, "object argument");
          Type FunctionInputObjectTy = checkLValue(FT->getInput(),
                                                   "'self' parameter");
          
          checkSameOrSubType(InputExprObjectTy, FunctionInputObjectTy,
                             "object argument and 'self' parameter");
        } else if (!TT || TT->getFields().size() != 1 ||
                   TT->getFields()[0].getType()->getCanonicalType()
                     != InputExprTy) {
          Out << "Argument type does not match parameter type in ApplyExpr:"
                 "\nArgument type: ";
          E->getArg()->getType().print(Out);
          Out << "\nParameter type: ";
          FT->getInput()->print(Out);
          Out << "\n";
          E->dump(Out);
          abort();
        }
      }

      bool looksLikeSuper = false;
      {
        Expr *arg = E->getArg()->getSemanticsProvidingExpr();
        while (auto ice = dyn_cast<ImplicitConversionExpr>(arg))
          arg = ice->getSubExpr()->getSemanticsProvidingExpr();
        looksLikeSuper = isa<SuperRefExpr>(arg);
      }
      if (E->isSuper() != looksLikeSuper) {
        Out << "Function application's isSuper() bit mismatch.\n";
        E->dump(Out);
        abort();
      }
      verifyCheckedBase(E);
    }

    void verifyChecked(MemberRefExpr *E) {
      if (!E->getMember()) {
        Out << "Member reference is missing declaration\n";
        E->dump(Out);
        abort();
      }
      
      // The only time the base is allowed to be inout is if we are accessing
      // a computed property.
      if (E->getBase()->getType()->is<InOutType>()) {
        VarDecl *VD = dyn_cast<VarDecl>(E->getMember().getDecl());
        if (!VD || !VD->hasAccessorFunctions()) {
          Out << "member_ref_expr on value of inout type\n";
          E->dump(Out);
          abort();
        }
      }
      
      // FIXME: Check container/member types through substitutions.

      verifyCheckedBase(E);
    }

    void verifyChecked(DynamicMemberRefExpr *E) {
      // The base type must be DynamicLookup.
      auto baseTy = E->getBase()->getType();

      // The base might be a metatype of DynamicLookup.
      if (auto baseMetaTy = baseTy->getAs<MetatypeType>()) {
        baseTy = baseMetaTy->getInstanceType();
      }

      auto baseProtoTy = baseTy->getAs<ProtocolType>();
      if (!baseProtoTy ||
          !baseProtoTy->getDecl()->isSpecificProtocol(
             KnownProtocolKind::DynamicLookup)) {
        Out << "Dynamic member reference has non-DynamicLookup base\n";
        E->dump(Out);
        abort();
      }

      // The member must be [objc].
      if (!E->getMember().getDecl()->isObjC()) {
        Out << "Dynamic member reference to non-[objc] member\n";
        E->dump(Out);
        abort();
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(SubscriptExpr *E) {
      if (!E->getDecl()) {
        Out << "Subscript expression is missing subscript declaration";
        abort();
      }

      // FIXME: Check base/member types through substitutions.

      verifyCheckedBase(E);
    }

    void verifyChecked(CheckedCastExpr *E) {
      if (!E->isResolved()) {
        Out << "CheckedCast kind not resolved\n";
        abort();
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(CoerceExpr *E) {
      checkSameType(E->getType(), E->getSubExpr()->getType(),
                    "coercion type and subexpression type");

      verifyCheckedBase(E);
    }

    void verifyChecked(TupleShuffleExpr *E) {
      TupleType *TT = E->getType()->getAs<TupleType>();
      TupleType *SubTT = E->getSubExpr()->getType()->getAs<TupleType>();
      if (!TT || !SubTT) {
        Out << "Unexpected types in TupleShuffleExpr\n";
        abort();
      }
      unsigned varargsStartIndex = 0;
      Type varargsType;
      unsigned callerDefaultArgIndex = 0;
      for (unsigned i = 0, e = E->getElementMapping().size(); i != e; ++i) {
        int subElem = E->getElementMapping()[i];
        if (subElem == TupleShuffleExpr::DefaultInitialize)
          continue;
        if (subElem == TupleShuffleExpr::FirstVariadic) {
          varargsStartIndex = i + 1;
          varargsType = TT->getFields()[i].getVarargBaseTy();
          break;
        }
        if (subElem == TupleShuffleExpr::CallerDefaultInitialize) {
          auto init = E->getCallerDefaultArgs()[callerDefaultArgIndex++];
          if (!TT->getElementType(i)->isEqual(init->getType())) {
            Out << "Type mismatch in TupleShuffleExpr\n";
            abort();
          }
          continue;
        }
        if (!TT->getElementType(i)->isEqual(SubTT->getElementType(subElem))) {
          Out << "Type mismatch in TupleShuffleExpr\n";
          abort();
        }
      }
      if (varargsStartIndex) {
        unsigned i = varargsStartIndex, e = E->getElementMapping().size();
        for (; i != e; ++i) {
          unsigned subElem = E->getElementMapping()[i];
          if (!SubTT->getElementType(subElem)->isEqual(varargsType)) {
            Out << "Vararg type mismatch in TupleShuffleExpr\n";
            abort();
          }
        }
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(MetatypeExpr *E) {
      auto metatype = E->getType()->getAs<MetatypeType>();
      if (!metatype) {
        Out << "MetatypeExpr must have metatype type\n";
        abort();
      }

      if (E->getBase()) {
        checkSameType(E->getBase()->getType(), metatype->getInstanceType(),
                      "base type of .metatype expression");
      }

      verifyCheckedBase(E);
    }

    void verifyParsed(NewArrayExpr *E) {
      if (E->getBounds().empty()) {
        Out << "NewArrayExpr has an empty bounds list\n";
        abort();
      }
      if (E->getBounds()[0].Value == nullptr) {
        Out << "First bound of NewArrayExpr is missing\n";
        abort();
      }
      verifyParsedBase(E);
    }

    void verifyChecked(NewArrayExpr *E) {
      if (!E->hasElementType()) {
        Out << "NewArrayExpr is missing its element type";
        abort();
      }

      if (!E->hasInjectionFunction()) {
        Out << "NewArrayExpr is missing an injection function";
        abort();
      }
      verifyCheckedBase(E);
    }

    void verifyChecked(InjectIntoOptionalExpr *expr) {
      auto valueType = expr->getType()->getAnyOptionalObjectType();
      if (!valueType) {
        Out << "InjectIntoOptionalExpr is not of Optional type";
        abort();
      }

      if (!expr->getSubExpr()->getType()->isEqual(valueType)) {
        Out << "InjectIntoOptionalExpr operand is not of the value type";
        abort();
      }
      verifyCheckedBase(expr);
    }

    void verifyChecked(IfExpr *expr) {
      auto condTy
        = expr->getCondExpr()->getType()->getAs<BuiltinIntegerType>();
      if (!condTy || !condTy->isFixedWidth() || condTy->getFixedWidth() != 1) {
        Out << "IfExpr condition is not an i1\n";
        abort();
      }

      checkSameType(expr->getThenExpr()->getType(),
                    expr->getElseExpr()->getType(),
                    "then and else branches of an if-expr");
      verifyCheckedBase(expr);
    }
    
    void verifyChecked(SuperRefExpr *expr) {
      verifyCheckedBase(expr);
    }

    void verifyChecked(ForceValueExpr *E) {
      auto valueTy = E->getType();
      auto optValueTy =
        E->getSubExpr()->getType()->getAnyOptionalObjectType();
      checkSameType(valueTy, optValueTy, "optional value type");
      verifyCheckedBase(E);
    }

    void verifyChecked(OpaqueValueExpr *expr) {
      if (!OpaqueValues.count(expr)) {
        Out << "OpaqueValueExpr not introduced at this point in AST\n";
        abort();
      }

      ++OpaqueValues[expr];

      // Make sure "uniquely-referenced" actually is.
      if (expr->isUniquelyReferenced() && OpaqueValues[expr] > 1) {
        Out << "Multiple references to unique OpaqueValueExpr\n";
        abort();
      }
      verifyCheckedBase(expr);
    }

    void verifyChecked(PatternBindingDecl *binding) {
      // Verify that a binding without storage declares a simple
      // variable without storage.
      if (!binding->hasStorage()) {
        auto pattern = binding->getPattern();
        if (auto typed = dyn_cast<TypedPattern>(pattern))
          pattern = typed->getSubPattern();
        auto named = dyn_cast<NamedPattern>(pattern);
        if (!named) {
          Out << "Unstored PatternBindingDecl with a non-simple pattern";
          abort();
        } else if (named->getDecl()->hasStorage()) {
          Out << "Unstored PatternBindingDecl declares variable with storage";
          abort();
        }
      } else {
        // TODO: verify that none of the bound variables has storage.
      }
    }

    void verifyChecked(VarDecl *var) {
      // The fact that this is *directly* be a reference storage type
      // cuts the code down quite a bit in getTypeOfReference.
      if (var->getAttrs().hasOwnership() !=
          isa<ReferenceStorageType>(var->getType().getPointer())) {
        if (var->getAttrs().hasOwnership()) {
          Out << "VarDecl has an ownership attribute, but its type"
                 " is not a ReferenceStorageType: ";
        } else {
          Out << "VarDecl has no ownership attribute, but its type"
                 " is a ReferenceStorageType: ";
        }
        var->getType().print(Out);
        abort();
      }
      verifyCheckedBase(var);
    }

    // Dump a reference to the given declaration.
    void dumpRef(Decl *decl) {
      if (auto value = dyn_cast<ValueDecl>(decl))
        value->dumpRef(Out);
      else if (auto ext = dyn_cast<ExtensionDecl>(decl)) {
        Out << "extension of ";
        if (ext->getExtendedType())
          ext->getExtendedType().print(Out);
      }
    }

    /// Check the given list of protocols.
    void verifyProtocolList(Decl *decl, ArrayRef<ProtocolDecl *> protocols) {
      // Make sure that the protocol list is fully expanded.
      SmallVector<ProtocolDecl *, 4> nominalProtocols(protocols.begin(),
                                                      protocols.end());
      ProtocolType::canonicalizeProtocols(nominalProtocols);

      SmallVector<Type, 4> protocolTypes;
      for (auto proto : protocols)
        protocolTypes.push_back(proto->getDeclaredType());
      SmallVector<ProtocolDecl *, 4> canonicalProtocols;
      ProtocolCompositionType::get(Ctx, protocolTypes)
        ->isExistentialType(canonicalProtocols);
      if (nominalProtocols != canonicalProtocols) {
        dumpRef(decl);
        Out << " doesn't have a complete set of protocols\n";
        abort();
      }      
    }

    /// Check the given explicit protocol conformance.
    void verifyConformance(Decl *decl, ProtocolConformance *conformance) {
      if (!conformance) {
        // FIXME: Eventually, this should itself be a verification
        // failure.
        return;
      }

      switch (conformance->getState()) {
      case ProtocolConformanceState::Complete:
      case ProtocolConformanceState::Invalid:
        // More checking below.
        break;
        
      case ProtocolConformanceState::Incomplete:
        dumpRef(decl);
        Out << " has a known-incomplete conformance for protocol "
            << conformance->getProtocol()->getName().str() << "\n";
        abort();
      }

      auto normal = dyn_cast<NormalProtocolConformance>(conformance);
      if (!normal)
        return;

      // Check that a normal protocol conformance is complete.
      auto proto = conformance->getProtocol();
      for (auto member : proto->getMembers()) {
        if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
          if (!normal->hasTypeWitness(assocType)) {
            dumpRef(decl);
            Out << " is missing type witness for "
                << conformance->getProtocol()->getName().str() 
                << "." << assocType->getName().str()
                << "\n";
            abort();
          }
          continue;
        }
        
        // If this is a getter/setter for a funcdecl, ignore it.
        if (auto *FD = dyn_cast<FuncDecl>(member))
          if (FD->isGetterOrSetter())
            continue;
        
        if (auto req = dyn_cast<ValueDecl>(member)) {
          if (!normal->hasWitness(req)) {
            dumpRef(decl);
            Out << " is missing witness for "
                << conformance->getProtocol()->getName().str() 
                << "." << req->getName().str()
                << "\n";
            abort();
          }
          continue;
        }
      }
    }
    
    void verifyChecked(NominalTypeDecl *nominal) {
      // Make sure that the protocol list is fully expanded.
      verifyProtocolList(nominal, nominal->getProtocols());

      // Make sure that the protocol conformances are complete.
      for (auto conformance : nominal->getConformances()) {
        verifyConformance(nominal, conformance);
      }

      verifyCheckedBase(nominal);
    }

    void verifyChecked(ExtensionDecl *ext) {
      // Make sure that the protocol list is fully expanded.
      verifyProtocolList(ext, ext->getProtocols());

      // Make sure that the protocol conformances are complete.
      for (auto conformance : ext->getConformances()) {
        verifyConformance(ext, conformance);
      }

      verifyCheckedBase(ext);
    }

    void verifyParsed(EnumElementDecl *UED) {
      if (!isa<EnumDecl>(UED->getDeclContext())) {
        Out << "EnumElementDecl has wrong DeclContext";
        abort();
      }

      verifyParsedBase(UED);
    }

    void verifyParsed(AbstractFunctionDecl *AFD) {
      if (AFD->getArgParamPatterns().size() !=
          AFD->getBodyParamPatterns().size()) {
        Out << "number of arg and body parameter patterns should be equal";
        abort();
      }

      if (AFD->hasSelectorStyleSignature()) {
        unsigned NumExpectedParamPatterns = 1;
        if (AFD->getImplicitSelfDecl())
          NumExpectedParamPatterns++;
        if (AFD->getArgParamPatterns().size() != NumExpectedParamPatterns) {
          Out << "functions with selector-style signature should "
                 "not be curried";
          abort();
        }
      }

      verifyParsedBase(AFD);
    }

    void verifyParsed(ConstructorDecl *CD) {
      if (CD->getArgParamPatterns().size() != 2 ||
          CD->getBodyParamPatterns().size() != 2) {
        Out << "ConstructorDecl should have exactly two parameter patterns";
        abort();
      }

      auto *DC = CD->getDeclContext();
      if (!isa<NominalTypeDecl>(DC) && !isa<ExtensionDecl>(DC) &&
          !CD->isInvalid()) {
        Out << "ConstructorDecls outside nominal types and extensions "
               "should be marked invalid";
        abort();
      }

      verifyParsedBase(CD);
    }

    void verifyParsed(ProtocolDecl *PD) {
      if (PD->isObjC() && !PD->requiresClass()) {
        Out << "@objc protocols should be class protocols as well";
        abort();
      }
      verifyParsedBase(PD);
    }

    void verifyChecked(ConstructorDecl *CD) {
      auto *ND = CD->getExtensionType()->getNominalOrBoundGenericNominal();
      if (!isa<ClassDecl>(ND) && !isa<StructDecl>(ND) && !isa<EnumDecl>(ND) &&
          !CD->isInvalid()) {
        Out << "ConstructorDecls outside structs, classes or enums"
               "should be marked invalid";
        abort();
      }
      verifyCheckedBase(CD);
    }

    void verifyParsed(DestructorDecl *DD) {
      if (DD->isGeneric()) {
        Out << "DestructorDecl can not be generic";
        abort();
      }
      if (DD->getArgParamPatterns().size() != 1 ||
          DD->getBodyParamPatterns().size() != 1) {
        Out << "DestructorDecl should have 'self' parameter pattern only";
        abort();
      }

      auto *DC = DD->getDeclContext();
      if (!isa<NominalTypeDecl>(DC) && !isa<ExtensionDecl>(DC) &&
          !DD->isInvalid()) {
        Out << "DestructorDecls outside nominal types and extensions "
               "should be marked invalid";
        abort();
      }

      if (DD->hasSelectorStyleSignature()) {
        Out << "DestructorDecls can not have a selector-style signature";
        abort();
      }

      verifyParsedBase(DD);
    }

    /// Check that the generic requirements line up with the archetypes.
    void checkGenericRequirements(Decl *decl,
                                  DeclContext *dc,
                                  GenericFunctionType *genericTy) {

      // We need to have generic parameters here.
      auto genericParams = dc->getGenericParamsOfContext();
      if (!genericParams) {
        Out << "Missing generic parameters\n";
        decl->dump(Out);
        abort();
      }

      // Step through the list of requirements in the generic type.
      auto requirements = genericTy->getRequirements();

      // Skip over same-type requirements.
      auto skipUnrepresentedRequirements = [&]() {
        for (; !requirements.empty(); requirements = requirements.slice(1)) {
          bool done = false;
          switch (requirements.front().getKind()) {
          case RequirementKind::Conformance:
            // If the second type is a protocol type, we're done.
            if (requirements.front().getSecondType()->is<ProtocolType>())
              done = true;

            break;

          case RequirementKind::SameType:
            // Skip the next same-type constraint.
            continue;

          case RequirementKind::WitnessMarker:
            done = true;
            break;
          }

          if (done)
            break;
        }
      };
      skipUnrepresentedRequirements();

      // Collect all of the generic parameter lists.
      SmallVector<GenericParamList *, 4> allGenericParamLists;
      for (auto gpList = genericParams; gpList;
           gpList = gpList->getOuterParameters()) {
        allGenericParamLists.push_back(gpList);
      }
      std::reverse(allGenericParamLists.begin(), allGenericParamLists.end());

      // Helpers that diagnose failures when generic requirements mismatch.
      bool failed = false;
      auto noteFailure =[&]() {
        if (failed)
          return;

        Out << "Generic requirements don't match all archetypes\n";
        decl->dump(Out);

        Out << "\nGeneric type: " << genericTy->getString() << "\n";
        Out << "Expected requirements: ";
        bool first = true;
        for (auto gpList : allGenericParamLists) {
          for (auto archetype : gpList->getAllArchetypes()) {
            for (auto proto : archetype->getConformsTo()) {
              if (first)
                first = false;
              else
                Out << ", ";

              Out << archetype->getString() << " : "
                  << proto->getDeclaredType()->getString();
            }
          }
        }
        Out << "\n";

        failed = true;
      };

      // Walk through all of the archetypes in the generic parameter lists,
      // matching up their conformance requirements with those in the
      for (auto gpList : allGenericParamLists) {
        for (auto archetype : gpList->getAllArchetypes()) {
          // Make sure we have the value witness marker.
          if (requirements.empty()) {
            noteFailure();
            Out << "Ran out of requirements before we ran out of archetypes\n";
            break;
          }

          if (requirements.front().getKind()
                == RequirementKind::WitnessMarker) {
            auto type = ArchetypeBuilder::mapTypeIntoContext(
                          dc,
                          requirements.front().getFirstType());
            if (type->isEqual(archetype)) {
              requirements = requirements.slice(1);
              skipUnrepresentedRequirements();
            } else {
              noteFailure();
              Out << "Value witness marker for " << type->getString()
                  << " does not match expected " << archetype->getString()
                  << "\n";
            }
          } else {
            noteFailure();
            Out << "Missing value witness marker for "
                << archetype->getString() << "\n";
          }

          for (auto proto : archetype->getConformsTo()) {
            // If there are no requirements left, we're missing requirements.
            if (requirements.empty()) {
              noteFailure();
              Out << "No requirement for " << archetype->getString()
                  << " : " << proto->getDeclaredType()->getString() << "\n";
              continue;
            }

            auto firstReqType = ArchetypeBuilder::mapTypeIntoContext(
                                  dc,
                                  requirements.front().getFirstType());
            auto secondReqType = ArchetypeBuilder::mapTypeIntoContext(
                                  dc,
                                  requirements.front().getSecondType());

            // If the requirements match up, move on to the next requirement.
            if (firstReqType->isEqual(archetype) &&
                secondReqType->isEqual(proto->getDeclaredType())) {
              requirements = requirements.slice(1);
              skipUnrepresentedRequirements();
              continue;
            }

            noteFailure();

            // If the requirements don't match up, complain.
            if (!firstReqType->isEqual(archetype)) {
              Out << "Mapped archetype " << firstReqType->getString()
                  << " does not match expected " << archetype->getString()
                  << "\n";
              continue;
            }

            Out << "Mapped conformance " << secondReqType->getString()
                << " does not match expected "
                << proto->getDeclaredType()->getString() << "\n";
          }
        }
      }

      if (!requirements.empty()) {
        noteFailure();
        Out << "Extra requirement "
            << requirements.front().getFirstType()->getString()
            << " : "
            << requirements.front().getSecondType()->getString()
            << "\n";
      }

      if (failed)
        abort();
    }

    void verifyChecked(AbstractFunctionDecl *AFD) {
      // If this function is generic or is within a generic type, it should
      // have an interface type.
      if ((AFD->getGenericParams() ||
           (AFD->getDeclContext()->isTypeContext() &&
            AFD->getDeclContext()->getGenericParamsOfContext()))
          && !AFD->getInterfaceType()->is<GenericFunctionType>()) {
        Out << "Missing interface type for generic function\n";
        AFD->dump(Out);
        abort();
      }

      // If there is an interface type, it shouldn't have any unresolved
      // dependent member types.
      // FIXME: This is a general property of the type system.
      auto interfaceTy = AFD->getInterfaceType();
      Type unresolvedDependentTy;
      interfaceTy.findIf([&](Type type) -> bool {
        if (auto dependent = type->getAs<DependentMemberType>()) {
          if (dependent->getAssocType() == nullptr) {
            unresolvedDependentTy = dependent;
            return true;
          }
        }
        return false;
      });

      if (unresolvedDependentTy) {
        Out << "Unresolved dependent member type ";
        unresolvedDependentTy->print(Out);
        abort();
      }

      // If the interface type is generic, make sure its requirements
      // line up with the archetypes.
      if (auto genericTy = interfaceTy->getAs<GenericFunctionType>()) {
        checkGenericRequirements(AFD, AFD, genericTy);
      }

      verifyCheckedBase(AFD);
    }

    void verifyChecked(DestructorDecl *DD) {
      auto *ND = DD->getExtensionType()->getNominalOrBoundGenericNominal();
      if (!isa<ClassDecl>(ND) && !DD->isInvalid()) {
        Out << "DestructorDecls outside classes should be marked invalid";
        abort();
      }
      verifyCheckedBase(DD);
    }

    void verifyChecked(FuncDecl *FD) {
      // FIXME: Chain to AbstractFunctionDecl checking!
    }

    void verifyParsed(FuncDecl *FD) {
      unsigned MinParamPatterns = FD->getImplicitSelfDecl() ? 2 : 1;
      if (FD->getArgParamPatterns().size() < MinParamPatterns ||
          FD->getBodyParamPatterns().size() < MinParamPatterns) {
        Out << "should have at least " << MinParamPatterns
            << " parameter patterns";
        abort();
      }

      if (FD->isAccessor()) {
        unsigned NumExpectedParamPatterns = 1;
        if (FD->getImplicitSelfDecl())
          NumExpectedParamPatterns++;
        if (FD->getArgParamPatterns().size() != NumExpectedParamPatterns) {
          Out << "accessors should not be curried";
          abort();
        }
      }

      if (auto *VD = FD->getAccessorStorageDecl()) {
        if (isa<VarDecl>(VD)
            && cast<VarDecl>(VD)->isStatic() != FD->isStatic()) {
          Out << "getter or setter static-ness must match static-ness of var";
          abort();
        }
      }

      verifyParsedBase(FD);
    }

    void verifyChecked(ClassDecl *CD) {
      if (!CD->hasLazyMembers()) {
        unsigned NumDestructors = 0;
        for (auto Member : CD->getMembers()) {
          if (isa<DestructorDecl>(Member)) {
            NumDestructors++;
          }
        }
        if (NumDestructors != 1) {
          Out << "every class should have exactly one destructor, "
                 "explicitly provided or created by the type checker";
          abort();
        }
      }

      verifyCheckedBase(CD);
    }

    void verifyParsed(AssociatedTypeDecl *ATD) {
      auto *DC = ATD->getDeclContext();
      if (!isa<NominalTypeDecl>(DC) ||
          !isa<ProtocolDecl>(cast<NominalTypeDecl>(DC))) {
        Out << "AssociatedTypeDecl should only occur inside a protocol";
        abort();
      }
      verifyParsedBase(ATD);
    }

    void verifyParsed(TuplePattern *TP) {
      if (TP->hasVararg()) {
        auto *LastPattern = TP->getFields().back().getPattern();
        if (!isa<TypedPattern>(LastPattern)) {
          Out << "a vararg subpattern of a TuplePattern should be"
                 "a TypedPattern";
          abort();
        }
      }
      verifyParsedBase(TP);
    }

    void verifyChecked(TuplePattern *TP) {
      if (TP->hasVararg()) {
        auto *LastPattern = TP->getFields().back().getPattern();
        Type T = cast<TypedPattern>(LastPattern)->getType()->getCanonicalType();
        if (auto *BGT = T->getAs<BoundGenericType>()) {
          if (BGT->getDecl() == Ctx.getSliceDecl())
            return;
        }
        Out << "a vararg subpattern of a TuplePattern has wrong type";
        abort();
      }
      verifyCheckedBase(TP);
    }

    /// Look through a possible l-value type, returning true if it was
    /// an l-value.
    bool lookThroughLValue(Type &type, bool &isInOut) {
      if (LValueType *lv = type->getAs<LValueType>()) {
        Type objectType = lv->getObjectType();
        if (objectType->is<LValueType>()) {
          Out << "type is an lvalue of lvalue type: ";
          type.print(Out);
          Out << "\n";
        }
        isInOut = false;
        type = objectType;
        return true;
      }
      if (InOutType *io = type->getAs<InOutType>()) {
        Type objectType = io->getObjectType();
        if (objectType->is<InOutType>()) {
          Out << "type is an inout of inout type: ";
          type.print(Out);
          Out << "\n";
        }
        isInOut = true;
        type = objectType;
        return true;
      }
      return false;
    }

    /// The two types are required to either both be l-values or
    /// both not be l-values.  They are adjusted to not be l-values.
    /// Returns true if they are both l-values.
    bool checkSameLValueness(Type &T0, Type &T1,
                             const char *what) {
      bool Q0, Q1;
      bool isLValue0 = lookThroughLValue(T0, Q0);
      bool isLValue1 = lookThroughLValue(T1, Q1);
      
      if (isLValue0 != isLValue1) {
        Out << "lvalue-ness of " << what << " do not match: "
            << isLValue0 << ", " << isLValue1 << "\n";
        abort();
      }

      if (isLValue0 && Q0 != Q1) {
        Out << "qualification of " << what << " do not match\n";
        abort();
      }

      return isLValue0;
    }

    Type checkLValue(Type T, const char *what) {
      LValueType *LV = T->getAs<LValueType>();
      if (LV)
        return LV->getObjectType();

      Out << "type is not an l-value in " << what << ": ";
      T.print(Out);
      Out << "\n";
      abort();
    }

    // Verification utilities.
    Type checkMetatypeType(Type type, const char *what) {
      auto metatype = type->getAs<MetatypeType>();
      if (metatype) return metatype->getInstanceType();

      Out << what << " is not a metatype: ";
      type.print(Out);
      Out << "\n";
      abort();
    }

    void checkIsTypeOfRValue(ValueDecl *D, Type rvalueType, const char *what) {
      auto declType = D->getType();
      if (auto refType = declType->getAs<ReferenceStorageType>())
        declType = refType->getReferentType();
      checkSameType(declType, rvalueType, what);
    }

    void checkSameType(Type T0, Type T1, const char *what) {
      if (T0->getCanonicalType() == T1->getCanonicalType())
        return;

      Out << "different types for " << what << ": ";
      T0.print(Out);
      Out << " vs. ";
      T1.print(Out);
      Out << "\n";
      abort();
    }

    void checkTrivialSubtype(Type srcTy, Type destTy, const char *what) {
      if (srcTy->isEqual(destTy)) return;

      if (auto srcMetatype = srcTy->getAs<MetatypeType>()) {
        if (auto destMetatype = destTy->getAs<MetatypeType>()) {
          return checkTrivialSubtype(srcMetatype->getInstanceType(),
                                     destMetatype->getInstanceType(),
                                     what);
        }
        goto fail;
      }

      // If the destination is a class, walk the supertypes of the source.
      if (destTy->getClassOrBoundGenericClass()) {
        if (!destTy->isSuperclassOf(srcTy, nullptr)) {
          srcTy.print(Out);
          Out << " is not a superclass of ";
          destTy.print(Out);
          Out << " for " << what << "\n";
          abort();
        }

        return;
      }

      // FIXME: Tighten up checking for conversions to protocol types.
      if (destTy->isExistentialType())
        return;

    fail:
      Out << "subtype conversion in " << what << " is invalid: ";
      srcTy.print(Out);
      Out << " to ";
      destTy.print(Out);
      Out << "\n";
      abort();
    }

    void checkSameOrSubType(Type T0, Type T1, const char *what) {
      if (T0->getCanonicalType() == T1->getCanonicalType())
        return;

      // Protocol subtyping.
      if (auto Proto0 = T0->getAs<ProtocolType>())
        if (auto Proto1 = T1->getAs<ProtocolType>())
          if (Proto0->getDecl()->inheritsFrom(Proto1->getDecl()))
            return;
      
      // FIXME: Actually check this?
      if (T0->isExistentialType() || T1->isExistentialType())
        return;
      
      Out << "incompatible types for " << what << ": ";
      T0.print(Out);
      Out << " vs. ";
      T1.print(Out);
      Out << "\n";
      abort();
    }

    bool isGoodSourceRange(SourceRange SR) {
      if (SR.isInvalid())
        return false;
      (void) Ctx.SourceMgr.findBufferContainingLoc(SR.Start);
      (void) Ctx.SourceMgr.findBufferContainingLoc(SR.End);
      return true;
    }

    void checkSourceRanges(Expr *E) {
      if (!E->getSourceRange().isValid()) {
        // We don't care about source ranges on implicitly-generated
        // expressions.
        if (E->isImplicit())
          return;

        Out << "invalid source range for expression: ";
        E->print(Out);
        Out << "\n";
        abort();
      }
      if (!isGoodSourceRange(E->getSourceRange())) {
        Out << "bad source range for expression: ";
        E->print(Out);
        Out << "\n";
        abort();
      }
      // FIXME: Re-visit this to always do the check.
      if (!E->isImplicit())
        checkSourceRanges(E->getSourceRange(), Parent,
                          [&]{ E->print(Out); } );
    }

    void checkSourceRanges(Stmt *S) {
      if (!S->getSourceRange().isValid()) {
        // We don't care about source ranges on implicitly-generated
        // statements.
        if (S->isImplicit())
          return;

        Out << "invalid source range for statement: ";
        S->print(Out);
        Out << "\n";
        abort();
      }
      if (!isGoodSourceRange(S->getSourceRange())) {
        Out << "bad source range for statement: ";
        S->print(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(S->getSourceRange(), Parent,
                        [&]{ S->print(Out); });
    }

    void checkSourceRanges(Pattern *P) {
      if (!P->getSourceRange().isValid()) {
        // We don't care about source ranges on implicitly-generated
        // patterns.
        if (P->isImplicit())
          return;

        Out << "invalid source range for pattern: ";
        P->print(Out);
        Out << "\n";
        abort();
      }
      if (!isGoodSourceRange(P->getSourceRange())) {
        Out << "bad source range for pattern: ";
        P->print(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(P->getSourceRange(), Parent,
                        [&]{ P->print(Out); });
    }

    void checkSourceRanges(Decl *D) {
      if (!D->getSourceRange().isValid()) {
        // We don't care about source ranges on implicitly-generated
        // decls.
        if (D->isImplicit())
          return;
        
        Out << "invalid source range for decl: ";
        D->print(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(D->getSourceRange(), Parent,
                        [&]{ D->print(Out); });
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        if (!VD->getTypeSourceRangeForDiagnostics().isValid()) {
          Out << "invalid type source range for variable decl: ";
          D->print(Out);
          Out << "\n";
          abort();
        }
      }
    }

    /// \brief Verify that the given source ranges is contained within the
    /// parent's source range.
    void checkSourceRanges(SourceRange Current,
                           ASTWalker::ParentTy Parent,
                           std::function<void()> printEntity) {
      SourceRange Enclosing;
      if (Parent.isNull())
          return;

      if (Parent.getAsModule()) {
        return;
      } else if (Decl *D = Parent.getAsDecl()) {
        Enclosing = D->getSourceRange();
        if (D->isImplicit())
          return;
        // FIXME: This is not working well for decl parents.
        return;
      } else if (Stmt *S = Parent.getAsStmt()) {
        Enclosing = S->getSourceRange();
        if (S->isImplicit())
          return;
      } else if (Pattern *P = Parent.getAsPattern()) {
        Enclosing = P->getSourceRange();
      } else if (Expr *E = Parent.getAsExpr()) {
        // FIXME: This hack is required because the inclusion check below
        // doesn't compares the *start* of the ranges, not the end of the
        // ranges.  In the case of an interpolated string literal expr, the
        // subexpressions are contained within the string token.  This means
        // that comparing the start of the string token to the end of an
        // embedded expression will fail.
        if (isa<InterpolatedStringLiteralExpr>(E))
          return;

        if (E->isImplicit())
          return;
        
        Enclosing = E->getSourceRange();
      } else if (TypeRepr *TyR = Parent.getAsTypeRepr()) {
        Enclosing = TyR->getSourceRange();
      } else {
        llvm_unreachable("impossible parent node");
      }
      
      if (!Ctx.SourceMgr.rangeContains(Enclosing, Current)) {
        Out << "child source range not contained within its parent: ";
        printEntity();
        Out << "\n  parent range: ";
        Enclosing.print(Out, Ctx.SourceMgr);
        Out << "\n  child range: ";
        Current.print(Out, Ctx.SourceMgr);
        Out << "\n";
        abort();
      }
    }

    void checkErrors(Expr *E) {}
    void checkErrors(Stmt *S) {}
    void checkErrors(Pattern *P) {}
    void checkErrors(Decl *D) {}
    void checkErrors(ValueDecl *D) {
      if (!D->hasType())
        return;
      if (D->isInvalid() && !D->getType()->is<ErrorType>()) {
        Out << "Invalid decl has non-error type!\n";
        D->dump(Out);
        abort();
      }
      if (D->getType()->is<ErrorType>() && !D->isInvalid()) {
        Out << "Valid decl has error type!\n";
        D->dump(Out);
        abort();
      }
    }
  };
}

void swift::verify(SourceFile &SF) {
  Verifier verifier(SF, &SF);
  SF.walk(verifier);
}

void swift::verify(Decl *D) {
  Verifier V = Verifier::forDecl(D);
  D->walk(V);
}

