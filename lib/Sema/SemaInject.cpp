//===--- SemaInject.cpp - Semantic Analysis for Injection -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic rules for the injection of declarations into
//  various declarative contexts.
//
//===----------------------------------------------------------------------===//

#include "TreeTransform.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/Template.h"
#include "clang/Sema/SemaInternal.h"

using namespace clang;

namespace clang {

/// A compile-time value along with its type.
struct TypedValue {
  TypedValue(QualType T, const APValue& V) : Type(T), Value(V) { }

  QualType Type;
  APValue Value;
};

enum InjectedDefType {
  InjectedDef_Field,
  InjectedDef_Method
};

/// Records information about a definition inside a fragment that must be
/// processed later. These are typically fields and methods.
struct InjectedDef {
  InjectedDef(const InjectedDefType& T, Decl *F, Decl *I) :
    Type(T), Fragment(F), Injected(I) { }

  InjectedDefType Type;

  /// The declaration within the fragment.
  Decl *Fragment;

  /// The injected declaration.
  Decl *Injected;
};

struct InjectionCapture {
  FieldDecl *Decl;
  APValue Value;

  InjectionCapture(FieldDecl *Decl, APValue Value)
    : Decl(Decl), Value(Value) { }
};

class InjectionContext;

/// An injection context. This is declared to establish a set of
/// substitutions during an injection.
class InjectionContext : public TreeTransform<InjectionContext> {
   using Base = TreeTransform<InjectionContext>;
public:
  InjectionContext(Sema &SemaRef) : Base(SemaRef), Modifiers() { }

  ASTContext &getContext() { return getSema().Context; }

  /// Detach the context from the semantics object. Returns this object for
  /// convenience.
  InjectionContext *Detach() {
    // Reset the declaration modifiers. They're already been applied and
    // must not apply to nested declarations in a definition.
    Modifiers = ReflectionModifiers();

    return this;
  }

  /// Re-attach the context to the context stack.
  void Attach() {
  }

  /// Adds a substitution from one declaration to another.
  void AddDeclSubstitution(const Decl *Old, Decl *New) {
    assert(TransformedLocalDecls.count(Old) == 0 && "Overwriting substitution");
    transformedLocalDecl(const_cast<Decl*>(Old), New);
  }

  /// Adds a substitution from a fragment placeholder to its
  /// (type) constant value.
  void AddPlaceholderSubstitution(Decl *Orig, QualType T, const APValue &V) {
    assert(isa<VarDecl>(Orig) && "Expected a variable declaration");
    assert(PlaceholderSubsts.count(Orig) == 0 && "Overwriting substitution");
    PlaceholderSubsts.try_emplace(Orig, T, V);
  }

  /// Adds substitutions for each placeholder in the fragment.
  /// The types and values are sourced from the fields of the reflection
  /// class and the captured values.
  void AddPlaceholderSubstitutions(const DeclContext *Fragment,
                                   const ArrayRef<InjectionCapture> &Captures) {
    assert(isa<CXXFragmentDecl>(Fragment) && "Context is not a fragment");

    auto PlaceIter = Fragment->decls_begin();
    auto PlaceIterEnd = Fragment->decls_end();
    auto CaptureIter = Captures.begin();
    auto CaptureIterEnd = Captures.end();

    while (PlaceIter != PlaceIterEnd && CaptureIter != CaptureIterEnd) {
      Decl *Var = *PlaceIter++;

      const InjectionCapture &IC = (*CaptureIter++);
      QualType Ty = IC.Decl->getType();
      APValue Val = IC.Value;

      AddPlaceholderSubstitution(Var, Ty, Val);
    }
  }

  /// Returns a replacement for D if a substitution has been registered or
  /// nullptr if no such replacement exists.
  Decl *GetDeclReplacement(Decl *D) {
    auto Iter = TransformedLocalDecls.find(D);
    if (Iter != TransformedLocalDecls.end())
      return Iter->second;
    else
      return nullptr;
  }

  /// Returns a replacement expression if E refers to a placeholder.
  Expr *GetPlaceholderReplacement(DeclRefExpr *E) {
    auto Iter = PlaceholderSubsts.find(E->getDecl());
    if (Iter != PlaceholderSubsts.end()) {
      // Build a new constant expression as the replacement. The source
      // expression is opaque since the actual declaration isn't part of
      // the output AST (but we might want it as context later -- makes
      // pretty printing more elegant).
      const TypedValue &TV = Iter->second;
      Expr *Opaque = new (getContext()) OpaqueValueExpr(
          E->getLocation(), TV.Type, VK_RValue, OK_Ordinary, E);
      return new (getContext()) CXXConstantExpr(Opaque, TV.Value);
    } else {
      return nullptr;
    }
  }

  /// Returns true if D is within an injected fragment or cloned declaration.
  bool IsInInjection(Decl *D);

  /// Sets the declaration modifiers.
  void SetModifiers(const ReflectionModifiers& Modifiers) {
    this->Modifiers = Modifiers;
  }

  DeclarationNameInfo TransformDeclarationName(NamedDecl *ND) {
    DeclarationNameInfo DNI(ND->getDeclName(), ND->getLocation());
    return TransformDeclarationNameInfo(DNI);
  }

  ExprResult TransformDeclRefExpr(DeclRefExpr *E) {
    if (Expr *R = GetPlaceholderReplacement(E)) {
      return R;
    }

    return Base::TransformDeclRefExpr(E);
  }

  bool InjectDeclarator(DeclaratorDecl *D, DeclarationNameInfo &DNI,
                        TypeSourceInfo *&TSI);
  bool InjectMemberDeclarator(DeclaratorDecl *D, DeclarationNameInfo &DNI,
                              TypeSourceInfo *&TSI, CXXRecordDecl *&Owner);

  void UpdateFunctionParms(FunctionDecl* Old, FunctionDecl* New);

  Decl *InjectNamespaceDecl(NamespaceDecl *D);
  Decl *InjectTypedefNameDecl(TypedefNameDecl *D);
  Decl *InjectFunctionDecl(FunctionDecl *D);
  Decl *InjectVarDecl(VarDecl *D);
  Decl *InjectCXXRecordDecl(CXXRecordDecl *D);
  Decl *InjectFieldDecl(FieldDecl *D);
  Decl *InjectCXXMethodDecl(CXXMethodDecl *D);
  Decl *InjectDeclImpl(Decl *D);
  Decl *InjectDecl(Decl *D);
  Decl *InjectAccessSpecDecl(AccessSpecDecl *D);
  Decl *InjectCXXMetaprogramDecl(CXXMetaprogramDecl *D);
  Decl *InjectCXXInjectionDecl(CXXInjectionDecl *D);

  TemplateParameterList *InjectTemplateParms(TemplateParameterList *Old);
  Decl *InjectFunctionTemplateDecl(FunctionTemplateDecl *D);
  Decl *InjectTemplateTypeParmDecl(TemplateTypeParmDecl *D);

  // Members

  /// A mapping of fragment placeholders to their typed compile-time
  /// values. This is used by TreeTransformer to replace references with
  /// constant expressions.
  llvm::DenseMap<Decl *, TypedValue> PlaceholderSubsts;

  /// A list of declarations whose definitions have not yet been
  /// injected. These are processed when a class receiving injections is
  /// completed.
  llvm::SmallVector<InjectedDef, 8> InjectedDefinitions;

  /// The modifiers to apply to injection.
  ReflectionModifiers Modifiers;
};

bool InjectionContext::IsInInjection(Decl *D) {
  return D->isInFragment();
}

// Inject the name and the type of a declarator declaration. Sets the
// declaration name info, type, and owner. Returns true if the declarator
// is invalid.
//
// FIXME: If the declarator has a nested names specifier, rebuild that
// also. That potentially modifies the owner of the declaration
bool InjectionContext::InjectDeclarator(DeclaratorDecl *D,
                                        DeclarationNameInfo &DNI,
                                        TypeSourceInfo *&TSI) {
  bool Invalid = false;

  // Rebuild the name.
  DNI = TransformDeclarationName(D);
  if (D->getDeclName().isEmpty() != DNI.getName().isEmpty()) {
    DNI = DeclarationNameInfo(D->getDeclName(), D->getLocation());
    Invalid = true;
  }

  // Rebuild the type.
  TSI = TransformType(D->getTypeSourceInfo());
  if (!TSI) {
    TSI = D->getTypeSourceInfo();
    Invalid = true;
  }

  return Invalid;
}

// Inject the name and the type of a declarator declaration. Sets the
// declaration name info, type, and owner. Returns true if the declarator
// is invalid.
bool InjectionContext::InjectMemberDeclarator(DeclaratorDecl *D,
                                              DeclarationNameInfo &DNI,
                                              TypeSourceInfo *&TSI,
                                              CXXRecordDecl *&Owner) {
  bool Invalid = InjectDeclarator(D, DNI, TSI);
  Owner = cast<CXXRecordDecl>(getSema().CurContext);
  return Invalid;
}

void InjectionContext::UpdateFunctionParms(FunctionDecl* Old,
                                           FunctionDecl* New) {
  // Make sure the parameters are actually bound to the function.
  TypeSourceInfo *TSI = New->getTypeSourceInfo();
  FunctionProtoTypeLoc TL = TSI->getTypeLoc().castAs<FunctionProtoTypeLoc>();
  New->setParams(TL.getParams());

  // Update the parameters their owning functions and register substitutions
  // as needed. Note that we automatically register substitutions for injected
  // parameters.
  unsigned OldIndex = 0;
  unsigned NewIndex = 0;
  auto OldParms = Old->parameters();
  auto NewParms = New->parameters();
  if (OldParms.size() > 0) {
    do {
      ParmVarDecl *OldParm = OldParms[OldIndex++];
      ParmVarDecl *NewParm = NewParms[NewIndex++];
      NewParm->setOwningFunction(New);
      AddDeclSubstitution(OldParm, NewParm);
    } while (OldIndex < OldParms.size() && NewIndex < NewParms.size());
  } else {
    assert(NewParms.size() == 0);
  }
  assert(OldIndex == OldParms.size() && NewIndex == NewParms.size());
}

Decl* InjectionContext::InjectNamespaceDecl(NamespaceDecl *D) {
  DeclContext *Owner = getSema().CurContext;

  // Build the namespace.
  //
  // FIXME: Search for a previous declaration of the namespace so that they
  // can be stitched together (i.e., redo lookup).
  NamespaceDecl *Ns = NamespaceDecl::Create(
      getContext(), Owner, D->isInline(), D->getLocation(), D->getLocation(),
      D->getIdentifier(), /*PrevDecl=*/nullptr);
  AddDeclSubstitution(D, Ns);

  Owner->addDecl(Ns);

  // Inject the namespace members.
  Sema::ContextRAII NsCxt(getSema(), Ns);
  for (Decl *OldMember : D->decls()) {
    Decl *NewMember = InjectDecl(OldMember);
    if (!NewMember || NewMember->isInvalidDecl())
      Ns->setInvalidDecl(true);
  }

  return Ns;
}

// Returns the default access specifier for the given decl context.
// This should be used with the destination decl context, not the
// original decl context, as we want to make sure we don't pick up
// the default access specifier of the source.
static AccessSpecifier GetDefaultAccessSpecifier(DeclContext *DC) {
  TagDecl *TD = cast<TagDecl>(DC);
  switch (TD->getTagKind()) {
  case TTK_Struct:
  case TTK_Union:
  case TTK_Enum:
    return AS_public;
  case TTK_Class:
    return AS_private;
  case TTK_Interface:
    break;
  }
  llvm_unreachable("Invalid tag kind");
}

static AccessSpecifier Transform(AccessModifier Modifier) {
  switch(Modifier) {
  case AccessModifier::Public:
    return AS_public;
  case AccessModifier::Protected:
    return AS_protected;
  case AccessModifier::Private:
    return AS_private;
  default:
    llvm_unreachable("Invalid access modifier transformation");
  }
}

template<typename T>
static void ApplyAccess(ReflectionModifiers Modifiers, T* Decl, T* OriginalDecl) {
  if (Modifiers.modifyAccess()) {
    AccessModifier Modifier = Modifiers.getAccessModifier();

    if (Modifier == AccessModifier::Default) {
      Decl->setAccess(GetDefaultAccessSpecifier(Decl->getDeclContext()));
      return;
    }

    Decl->setAccess(Transform(Modifier));
    return;
  }

  Decl->setAccess(OriginalDecl->getAccess());
}

Decl* InjectionContext::InjectTypedefNameDecl(TypedefNameDecl *D) {
  bool Invalid = false;

  DeclContext *Owner = getSema().CurContext;

  // Transform the type. If this fails, just retain the original, but
  // invalidate the declaration later.
  TypeSourceInfo *TSI = TransformType(D->getTypeSourceInfo());
  if (!TSI) {
    TSI = D->getTypeSourceInfo();
    Invalid = true;
  }

  // Create the new typedef
  TypedefNameDecl *Typedef;
  if (isa<TypeAliasDecl>(D))
    Typedef = TypeAliasDecl::Create(
        getContext(), Owner, D->getBeginLoc(), D->getLocation(),
        D->getIdentifier(), TSI);
  else
    Typedef = TypedefDecl::Create(
        getContext(), Owner, D->getBeginLoc(), D->getLocation(),
        D->getIdentifier(), TSI);
  AddDeclSubstitution(D, Typedef);

  ApplyAccess(Modifiers, Typedef, D);
  Typedef->setInvalidDecl(Invalid);
  Owner->addDecl(Typedef);

  return Typedef;
}

static bool InjectVariableInitializer(InjectionContext &Cxt,
                                      VarDecl *Old,
                                      VarDecl *New) {
  if (Old->getInit()) {
    if (New->isStaticDataMember() && !Old->isOutOfLine())
      Cxt.getSema().PushExpressionEvaluationContext(
          Sema::ExpressionEvaluationContext::ConstantEvaluated, Old);
    else
      Cxt.getSema().PushExpressionEvaluationContext(
          Sema::ExpressionEvaluationContext::PotentiallyEvaluated, Old);

    // Instantiate the initializer.
    ExprResult Init;
    {
      Sema::ContextRAII SwitchContext(Cxt.getSema(), New->getDeclContext());
      bool DirectInit = (Old->getInitStyle() == VarDecl::CallInit);
      Init = Cxt.TransformInitializer(Old->getInit(), DirectInit);
    }

    if (!Init.isInvalid()) {
      Expr *InitExpr = Init.get();
      if (New->hasAttr<DLLImportAttr>() &&
          (!InitExpr ||
           !InitExpr->isConstantInitializer(Cxt.getContext(), false))) {
        // Do not dynamically initialize dllimport variables.
      } else if (InitExpr) {
        Cxt.getSema().AddInitializerToDecl(New, InitExpr, Old->isDirectInit());
      } else {
        Cxt.getSema().ActOnUninitializedDecl(New);
      }
    } else {
      New->setInvalidDecl();
    }

    Cxt.getSema().PopExpressionEvaluationContext();
  } else {
    if (New->isStaticDataMember()) {
      if (!New->isOutOfLine())
        return New;

      // If the declaration inside the class had an initializer, don't add
      // another one to the out-of-line definition.
      if (Old->getFirstDecl()->hasInit())
        return New;
    }

    // We'll add an initializer to a for-range declaration later.
    if (New->isCXXForRangeDecl())
      return New;

    Cxt.getSema().ActOnUninitializedDecl(New);
  }

  return New;
}

Decl *InjectionContext::InjectFunctionDecl(FunctionDecl *D) {
  DeclContext *Owner = getSema().CurContext;

  DeclarationNameInfo DNI;
  TypeSourceInfo* TSI;
  bool Invalid = InjectDeclarator(D, DNI, TSI);

  // FIXME: Check for redeclaration.

  FunctionDecl* Fn = FunctionDecl::Create(
      getContext(), Owner, D->getLocation(), DNI, TSI->getType(), TSI,
      D->getStorageClass(), D->hasWrittenPrototype(), D->isConstexpr());
  AddDeclSubstitution(D, Fn);
  UpdateFunctionParms(D, Fn);

  // Set properties.
  Fn->setInlineSpecified(D->isInlineSpecified());
  Fn->setInvalidDecl(Invalid);

  Owner->addDecl(Fn);

  // If the function has a body, inject that also. Note that namespace-scope
  // function definitions are never deferred. Also, function decls never
  // appear in class scope (we hope), so we shouldn't be doing this too
  // early.
  if (Stmt *OldBody = D->getBody()) {
    Sema::SynthesizedFunctionScope Scope(getSema(), Fn);
    Sema::ContextRAII FnCxt (getSema(), Fn);
    StmtResult NewBody = TransformStmt(OldBody);
    if (NewBody.isInvalid())
      Fn->setInvalidDecl();
    else
      Fn->setBody(NewBody.get());
  }

  return Fn;
}

Decl *InjectionContext::InjectVarDecl(VarDecl *D) {
  DeclContext *Owner = getSema().CurContext;

  DeclarationNameInfo DNI;
  TypeSourceInfo *TSI;
  bool Invalid = InjectDeclarator(D, DNI, TSI);

  // FIXME: Check for re-declaration.

  VarDecl *Var = VarDecl::Create(
      getContext(), Owner, D->getInnerLocStart(), DNI, TSI->getType(),
      TSI, D->getStorageClass());
  AddDeclSubstitution(D, Var);

  if (D->isNRVOVariable()) {
    QualType ReturnType = cast<FunctionDecl>(Owner)->getReturnType();
    if (getSema().isCopyElisionCandidate(ReturnType, Var, Sema::CES_Strict))
      Var->setNRVOVariable(true);
  }

  Var->setImplicit(D->isImplicit());
  Var->setInvalidDecl(Invalid);
  Owner->addDecl(Var);

  // If we are instantiating a local extern declaration, the
  // instantiation belongs lexically to the containing function.
  // If we are instantiating a static data member defined
  // out-of-line, the instantiation will have the same lexical
  // context (which will be a namespace scope) as the template.
  if (D->isLocalExternDecl()) {
    Var->setLocalExternDecl();
    Var->setLexicalDeclContext(Owner);
  } else if (D->isOutOfLine()) {
    Var->setLexicalDeclContext(D->getLexicalDeclContext());
  }
  Var->setTSCSpec(D->getTSCSpec());
  Var->setInitStyle(D->getInitStyle());
  Var->setCXXForRangeDecl(D->isCXXForRangeDecl());
  Var->setConstexpr(D->isConstexpr());
  Var->setInitCapture(D->isInitCapture());
  Var->setPreviousDeclInSameBlockScope(D->isPreviousDeclInSameBlockScope());
  Var->setAccess(D->getAccess());

  if (!D->isStaticDataMember()) {
    if (D->isUsed(false))
      Var->setIsUsed();
    Var->setReferenced(D->isReferenced());
  }

  // FIXME: Instantiate attributes.

  // Forward the mangling number from the template to the instantiated decl.
  getContext().setManglingNumber(
      Var, getContext().getManglingNumber(D));
  getContext().setStaticLocalNumber(
      Var, getContext().getStaticLocalNumber(D));

  if (D->isInlineSpecified())
    Var->setInlineSpecified();
  else if (D->isInline())
    Var->setImplicitlyInline();

  InjectVariableInitializer(*this, D, Var);

  return Var;
}

/// Injects the base specifier Base into Class.
static bool InjectBaseSpecifiers(InjectionContext &Cxt, 
                                 CXXRecordDecl *OldClass,
                                 CXXRecordDecl *NewClass) {
  bool Invalid = false;
  SmallVector<CXXBaseSpecifier*, 4> Bases;
  for (const CXXBaseSpecifier &OldBase : OldClass->bases()) {
    TypeSourceInfo *TSI = Cxt.TransformType(OldBase.getTypeSourceInfo());
    if (!TSI) {
      Invalid = true;
      continue;
    }

    CXXBaseSpecifier *NewBase = Cxt.getSema().CheckBaseSpecifier(
        NewClass, OldBase.getSourceRange(), OldBase.isVirtual(), 
        OldBase.getAccessSpecifierAsWritten(), TSI, OldBase.getEllipsisLoc());
    if (!NewBase) {
      Invalid = true;
      continue;
    }

    Bases.push_back(NewBase);
  }

  if (!Invalid && Cxt.getSema().AttachBaseSpecifiers(NewClass, Bases))
    Invalid = true;

  // Invalidate the class if necessary.
  NewClass->setInvalidDecl(Invalid);

  return Invalid;
}

static bool InjectClassMembers(InjectionContext &Cxt,
                               CXXRecordDecl *OldClass,
                               CXXRecordDecl *NewClass) {
  for (Decl *OldMember : OldClass->decls()) {
    // Don't transform invalid declarations.
    if (OldMember->isInvalidDecl())
      continue;

    // Don't transform non-members appearing in a class.
    //
    // FIXME: What does it mean to inject friends?
    if (OldMember->getDeclContext() != OldClass)
      continue;

    Decl *NewMember = Cxt.InjectDecl(OldMember);
    if (!NewMember)
      NewClass->setInvalidDecl();
  }
  return NewClass->isInvalidDecl();
}

static bool InjectClassDefinition(InjectionContext &Cxt,
                                  CXXRecordDecl *OldClass,
                                  CXXRecordDecl *NewClass) {
  Sema::ContextRAII SwitchContext(Cxt.getSema(), NewClass);
  Cxt.getSema().StartDefinition(NewClass);
  InjectBaseSpecifiers(Cxt, OldClass, NewClass);
  InjectClassMembers(Cxt, OldClass, NewClass);
  Cxt.getSema().CompleteDefinition(NewClass);
  return NewClass->isInvalidDecl();
}

Decl *InjectionContext::InjectCXXRecordDecl(CXXRecordDecl *D) {
  bool Invalid = false;
  DeclContext *Owner = getSema().CurContext;

  // FIXME: Do a lookup for previous declarations.

  CXXRecordDecl *Class;
  if (D->isInjectedClassName()) {
    DeclarationName DN = cast<CXXRecordDecl>(Owner)->getDeclName();
    Class = CXXRecordDecl::Create(
        getContext(), D->getTagKind(), Owner, D->getBeginLoc(),
        D->getLocation(), DN.getAsIdentifierInfo(), /*PrevDecl=*/nullptr);
  } else {
    DeclarationNameInfo DNI = TransformDeclarationName(D);
    if (!DNI.getName())
      Invalid = true;
    Class = CXXRecordDecl::Create(
        getContext(), D->getTagKind(), Owner, D->getBeginLoc(),
        D->getLocation(), DNI.getName().getAsIdentifierInfo(),
        /*PrevDecl=*/nullptr);
  }
  AddDeclSubstitution(D, Class);

  // FIXME: Inject attributes.

  // FIXME: Propagate other properties?
  Class->setAccess(D->getAccess());
  Class->setImplicit(D->isImplicit());
  Class->setInvalidDecl(Invalid);
  Owner->addDecl(Class);

  if (D->hasDefinition())
    InjectClassDefinition(*this, D, Class);

  return Class;
}

Decl *InjectionContext::InjectFieldDecl(FieldDecl *D) {
  DeclarationNameInfo DNI;
  TypeSourceInfo *TSI;
  CXXRecordDecl *Owner;
  bool Invalid = InjectMemberDeclarator(D, DNI, TSI, Owner);

  // FIXME: Substitute through the bit width.
  Expr *BitWidth = nullptr;

  // Build and check the field.
  FieldDecl *Field = getSema().CheckFieldDecl(
      DNI.getName(), TSI->getType(), TSI, Owner, D->getLocation(),
      D->isMutable(), BitWidth, D->getInClassInitStyle(), D->getInnerLocStart(),
      D->getAccess(), nullptr);
  AddDeclSubstitution(D, Field);

  // FIXME: Propagate attributes?

  // FIXME: In general, see VisitFieldDecl in the template instantiatior.
  // There are some interesting cases we probably need to handle.

  // Propagate semantic properties.
  Field->setImplicit(D->isImplicit());
  ApplyAccess(Modifiers, Field, D);

  if (!Field->isInvalidDecl())
    Field->setInvalidDecl(Invalid);

  Owner->addDecl(Field);

  // If the field has an initializer, add it to the Fragment so that we
  // can process it later.
  if (D->hasInClassInitializer())
    InjectedDefinitions.push_back(InjectedDef(InjectedDef_Field, D, Field));

  return Field;
}

Decl *InjectionContext::InjectCXXMethodDecl(CXXMethodDecl *D) {
  ASTContext &AST = getContext();
  DeclarationNameInfo DNI;
  TypeSourceInfo *TSI;
  CXXRecordDecl *Owner;
  bool Invalid = InjectMemberDeclarator(D, DNI, TSI, Owner);

  // Build the underlying method.
  //
  // FIXME: Should we propagate implicit operators?
  CXXMethodDecl *Method;
  if (CXXConstructorDecl *Ctor = dyn_cast<CXXConstructorDecl>(D)) {
    Method = CXXConstructorDecl::Create(AST, Owner, D->getBeginLoc(), DNI,
                                        TSI->getType(), TSI,
                                        Ctor->isExplicit(),
                                        Ctor->isInlineSpecified(),
                                        Ctor->isImplicit(),
                                        Ctor->isConstexpr());
    Method->setRangeEnd(D->getEndLoc());
  } else if (CXXDestructorDecl *Dtor = dyn_cast<CXXDestructorDecl>(D)) {
    Method = CXXDestructorDecl::Create(AST, Owner, D->getBeginLoc(), DNI,
                                       TSI->getType(), TSI,
                                       Dtor->isInlineSpecified(),
                                       Dtor->isImplicit());
    Method->setRangeEnd(D->getEndLoc());
  } else if (CXXConversionDecl *Conv = dyn_cast<CXXConversionDecl>(D)) {
    Method = CXXConversionDecl::Create(AST, Owner, D->getBeginLoc(), DNI,
                                       TSI->getType(), TSI,
                                       Conv->isInlineSpecified(),
                                       Conv->isExplicit(), Conv->isConstexpr(),
                                       Conv->getEndLoc());
  } else {
    Method = CXXMethodDecl::Create(AST, Owner, D->getBeginLoc(), DNI,
                                   TSI->getType(), TSI,
                                   D->isStatic() ? SC_Static : SC_None,
                                   D->isInlineSpecified(), D->isConstexpr(),
                                   D->getEndLoc());
  }
  AddDeclSubstitution(D, Method);
  UpdateFunctionParms(D, Method);

  // Propagate Template Attributes
  MemberSpecializationInfo *MemberSpecInfo = D->getMemberSpecializationInfo();
  if (MemberSpecInfo) {
    FunctionDecl *TemplateFD =
        static_cast<FunctionDecl *>(MemberSpecInfo->getInstantiatedFrom());
    TemplateSpecializationKind TemplateSK =
        MemberSpecInfo->getTemplateSpecializationKind();
    Method->setInstantiationOfMemberFunction(TemplateFD, TemplateSK);
  }

  // Propagate semantic properties.
  Method->setImplicit(D->isImplicit());
  ApplyAccess(Modifiers, Method, D);

  // Propagate virtual flags.
  Method->setVirtualAsWritten(D->isVirtualAsWritten());
  if (D->isPure())
    SemaRef.CheckPureMethod(Method, Method->getSourceRange());

  Method->setDeletedAsWritten(D->isDeletedAsWritten());
  Method->setDefaulted(D->isDefaulted());

  if (!Method->isInvalidDecl())
    Method->setInvalidDecl(Invalid);

  // Don't register the declaration if we're injecting the declaration of
  // a template-declaration. We'll add the template later.
  if (!D->getDescribedFunctionTemplate())
    Owner->addDecl(Method);

  // If the method is has a body, add it to the context so that we can
  // process it later. Note that deleted/defaulted definitions are just
  // flags processed above. Ignore the definition if we've marked this
  // as pure virtual.
  if (D->hasBody() && !Method->isPure())
    InjectedDefinitions.push_back(InjectedDef(InjectedDef_Method, D, Method));

  return Method;
}

Decl *InjectionContext::InjectDeclImpl(Decl *D) {
  // Inject the declaration.
  switch (D->getKind()) {
  case Decl::Namespace:
    return InjectNamespaceDecl(cast<NamespaceDecl>(D));
  case Decl::Typedef:
  case Decl::TypeAlias:
    return InjectTypedefNameDecl(cast<TypedefNameDecl>(D));
  case Decl::Function:
    return InjectFunctionDecl(cast<FunctionDecl>(D));
  case Decl::Var:
    return InjectVarDecl(cast<VarDecl>(D));
  case Decl::CXXRecord:
    return InjectCXXRecordDecl(cast<CXXRecordDecl>(D));
  case Decl::Field:
    return InjectFieldDecl(cast<FieldDecl>(D));
  case Decl::CXXMethod:
  case Decl::CXXConstructor:
  case Decl::CXXDestructor:
  case Decl::CXXConversion:
    return InjectCXXMethodDecl(cast<CXXMethodDecl>(D));
  case Decl::AccessSpec:
    return InjectAccessSpecDecl(cast<AccessSpecDecl>(D));
  case Decl::CXXMetaprogram:
    return InjectCXXMetaprogramDecl(cast<CXXMetaprogramDecl>(D));
  case Decl::CXXInjection:
    return InjectCXXInjectionDecl(cast<CXXInjectionDecl>(D));
  case Decl::FunctionTemplate:
    return InjectFunctionTemplateDecl(cast<FunctionTemplateDecl>(D));
  case Decl::TemplateTypeParm:
    return InjectTemplateTypeParmDecl(cast<TemplateTypeParmDecl>(D));
  default:
    break;
  }
  D->dump();
  llvm_unreachable("unhandled declaration");
}

/// Injects a new version of the declaration. Do not use this to
/// resolve references to declarations; use ResolveDecl instead.
Decl *InjectionContext::InjectDecl(Decl *D) {
  assert(!GetDeclReplacement(D) && "Declaration already injected");

  // If the declaration does not appear in the context, then it need
  // not be resolved.
  // if (!IsInInjection(D))
    // return D;

  Decl* R = InjectDeclImpl(D);
  if (!R)
    return nullptr;

  // If we injected a top-level declaration, notify the AST consumer,
  // so that it can be processed for code generation.
  if (isa<TranslationUnitDecl>(R->getDeclContext()))
    getSema().Consumer.HandleTopLevelDecl(DeclGroupRef(R));

  return R;
}

Decl *InjectionContext::InjectAccessSpecDecl(AccessSpecDecl *D) {
  CXXRecordDecl *Owner = cast<CXXRecordDecl>(getSema().CurContext);
  return AccessSpecDecl::Create(
      getContext(), D->getAccess(), Owner, D->getLocation(), D->getColonLoc());
}

template <typename MetaType>
static Decl *
InjectCXXMetaDecl(InjectionContext &Ctx, MetaType *D) {
  Sema &Sema = Ctx.getSema();

  // We can use the ActOn* members since the initial parsing for these
  // declarations is trivial (i.e., don't have to translate declarators).
  unsigned ScopeFlags; // Unused
  Decl *New = Sema.ActOnCXXMetaprogramDecl(/*Scope=*/nullptr, D->getLocation(),
                                           ScopeFlags);

  Sema.ActOnStartCXXMetaprogramDecl(/*Scope=*/nullptr, New);
  StmtResult S = Ctx.TransformStmt(D->getBody());
  if (!S.isInvalid())
    Sema.ActOnFinishCXXMetaprogramDecl(/*Scope=*/nullptr, New, S.get());
  else
    Sema.ActOnCXXMetaprogramDeclError(/*Scope=*/nullptr, New);

  return New;
}

Decl *InjectionContext::InjectCXXMetaprogramDecl(CXXMetaprogramDecl *D) {
  return InjectCXXMetaDecl(*this, D);
}

Decl *InjectionContext::InjectCXXInjectionDecl(CXXInjectionDecl *D) {
  return InjectCXXMetaDecl(*this, D);
}

TemplateParameterList *
InjectionContext::InjectTemplateParms(TemplateParameterList *OldParms) {
  bool Invalid = false;
  SmallVector<NamedDecl *, 8> NewParms;
  NewParms.reserve(OldParms->size());
  for (auto &P : *OldParms) {
    NamedDecl *D = cast_or_null<NamedDecl>(InjectDecl(P));
    NewParms.push_back(D);
    if (!D || D->isInvalidDecl())
      Invalid = true;
  }

  // Clean up if we had an error.
  if (Invalid)
    return nullptr;

  ExprResult Reqs = TransformExpr(OldParms->getRequiresClause());
  if (Reqs.isInvalid())
    return nullptr;

  return TemplateParameterList::Create(
      getSema().Context, OldParms->getTemplateLoc(), OldParms->getLAngleLoc(),
      NewParms, OldParms->getRAngleLoc(), Reqs.get());
}

Decl* InjectionContext::InjectFunctionTemplateDecl(FunctionTemplateDecl *D) {
  DeclContext *Owner = getSema().CurContext;

  TemplateParameterList *Parms = InjectTemplateParms(D->getTemplateParameters());
  if (!Parms)
    return nullptr;

  // Build the underlying pattern.
  Decl *Pattern = InjectDecl(D->getTemplatedDecl());
  if (!Pattern)
    return nullptr;
  FunctionDecl *Fn = cast<FunctionDecl>(Pattern);

  // Build the enclosing template.
  FunctionTemplateDecl *Template = FunctionTemplateDecl::Create(
      getSema().Context, getSema().CurContext, Fn->getLocation(),
      Fn->getDeclName(), Parms, Fn);
  AddDeclSubstitution(D, Template);

  // FIXME: Other attributes to process?
  Fn->setDescribedFunctionTemplate(Template);
  Template->setAccess(D->getAccess());

  // Add the declaration.
  Owner->addDecl(Template);

  return Template;
}

Decl* InjectionContext::InjectTemplateTypeParmDecl(TemplateTypeParmDecl *D) {
  TemplateTypeParmDecl *Parm = TemplateTypeParmDecl::Create(
      getSema().Context, getSema().CurContext, D->getBeginLoc(), D->getLocation(),
      D->getDepth(), D->getIndex(), D->getIdentifier(),
      D->wasDeclaredWithTypename(), D->isParameterPack());
  AddDeclSubstitution(D, Parm);

  Parm->setAccess(AS_public);

  // Process the default argument.
  if (D->hasDefaultArgument() && !D->defaultArgumentWasInherited()) {
    TypeSourceInfo *Default = TransformType(D->getDefaultArgumentInfo());
    if (Default)
      Parm->setDefaultArgument(Default);
    // FIXME: What if this fails.
  }

  return Parm;
}

} // namespace clang

// Find variables to capture in the given scope.
static void FindCapturesInScope(Sema &SemaRef, Scope *S,
                                SmallVectorImpl<VarDecl *> &Vars) {
  for (Decl *D : S->decls()) {
    if (VarDecl *Var = dyn_cast<VarDecl>(D)) {
      // Only capture locals with initializers.
      //
      // FIXME: If the fragment is in the initializer of a variable, this
      // will also capture that variable. For example:
      //
      //    auto f = <<class: ... >>;
      //
      // The capture list for the fragment will include f. This seems insane,
      // but lambda capture seems to also do this (with some caveats about
      // usage).
      //
      // We can actually detect this case in this implementation because
      // the type must be deduced and we won't have associated the
      // initializer with the variable yet.
      if (!isa<ParmVarDecl>(Var) &&
          !Var->hasInit() &&
          Var->getType()->isUndeducedType())
        continue;

      Vars.push_back(Var);
    }
  }
}

// Search the scope list for captured variables. When S is null, we're
// applying applying a transformation.
static void FindCaptures(Sema &SemaRef, Scope *S, FunctionDecl *Fn,
                         SmallVectorImpl<VarDecl *> &Vars) {
  assert(S && "Expected non-null scope");
  while (S && S->getEntity() != Fn) {
    FindCapturesInScope(SemaRef, S, Vars);
    S = S->getParent();
  }
  if (S)
    FindCapturesInScope(SemaRef, S, Vars);
}

/// Construct a reference to each captured value and force an r-value
/// conversion so that we get rvalues during evaluation.
static void ReferenceCaptures(Sema &SemaRef,
                              SmallVectorImpl<VarDecl *> &Vars,
                              SmallVectorImpl<Expr *> &Refs) {
  Refs.resize(Vars.size());
  std::transform(Vars.begin(), Vars.end(), Refs.begin(), [&](VarDecl *D) {
    Expr *Ref = new (SemaRef.Context) DeclRefExpr(D, false, D->getType(),
                                                  VK_LValue, D->getLocation());
    return ImplicitCastExpr::Create(SemaRef.Context, D->getType(),
                                    CK_LValueToRValue, Ref, nullptr, VK_RValue);
  });
}

/// Returns the variable from a captured declaration.
static VarDecl *GetVariableFromCapture(Expr *E) {
  Expr *Ref = cast<ImplicitCastExpr>(E)->getSubExpr();
  return cast<VarDecl>(cast<DeclRefExpr>(Ref)->getDecl());
}

// Create a placeholder for each captured expression in the scope of the
// fragment. For some captured variable 'v', these have the form:
//
//    constexpr auto v = <opaque>;
//
// These are replaced by their values during injection.
static void CreatePlaceholder(Sema &SemaRef, CXXFragmentDecl *Frag, Expr *E) {
  ValueDecl *Var = GetVariableFromCapture(E);
  SourceLocation IdLoc = Var->getLocation();
  IdentifierInfo *Id = Var->getIdentifier();
  QualType T = SemaRef.Context.DependentTy;
  TypeSourceInfo *TSI = SemaRef.Context.getTrivialTypeSourceInfo(T);
  VarDecl *Placeholder = VarDecl::Create(SemaRef.Context, Frag, IdLoc, IdLoc,
                                         Id, T, TSI, SC_Static);
  Placeholder->setConstexpr(true);
  Placeholder->setImplicit(true);
  Placeholder->setInitStyle(VarDecl::CInit);
  Placeholder->setInit(
      new (SemaRef.Context) OpaqueValueExpr(IdLoc, T, VK_RValue));
  Placeholder->setReferenced(true);
  Placeholder->markUsed(SemaRef.Context);
  Frag->addDecl(Placeholder);
}

static void CreatePlaceholders(Sema &SemaRef, CXXFragmentDecl *Frag,
                               SmallVectorImpl<Expr *> &Captures) {
  std::for_each(Captures.begin(), Captures.end(), [&](Expr *E) {
    CreatePlaceholder(SemaRef, Frag, E);
  });
}

/// Called at the start of a source code fragment to establish the list of
/// automatic variables captured. This is only called by the parser and searches
/// the list of local variables in scope.
void Sema::ActOnCXXFragmentCapture(SmallVectorImpl<Expr *> &Captures) {
  assert(Captures.empty() && "Captures already specified");

  // Only collect captures within a function.
  //
  // FIXME: It might be better to use the scope, but the flags don't appear
  // to be set right within constexpr declarations, etc.
  if (isa<FunctionDecl>(CurContext)) {
    SmallVector<VarDecl *, 8> Vars;
    FindCaptures(*this, CurScope, getCurFunctionDecl(), Vars);
    ReferenceCaptures(*this, Vars, Captures);
  }
}

/// Called at the start of a source code fragment to establish the fragment
/// declaration and placeholders.
Decl *Sema::ActOnStartCXXFragment(Scope* S, SourceLocation Loc,
                                  SmallVectorImpl<Expr *> &Captures) {
  CXXFragmentDecl *Fragment = CXXFragmentDecl::Create(Context, CurContext, Loc);
  CreatePlaceholders(*this, Fragment, Captures);

  if (S)
    PushDeclContext(S, Fragment);

  return Fragment;
}


/// Binds the content the fragment declaration. Returns the updated fragment.
/// The Fragment is nullptr if an error occurred during parsing. However,
/// we still need to pop the declaration context.
Decl *Sema::ActOnFinishCXXFragment(Scope *S, Decl *Fragment, Decl *Content) {
  CXXFragmentDecl *FD = nullptr;
  if (Fragment) {
    FD = cast<CXXFragmentDecl>(Fragment);
    FD->setContent(Content);
  }

  if (S)
    PopDeclContext();

  return FD;
}


/// Builds a new fragment expression.
ExprResult Sema::ActOnCXXFragmentExpr(SourceLocation Loc, Decl *Fragment,
                                      SmallVectorImpl<Expr *> &Captures) {
  return BuildCXXFragmentExpr(Loc, Fragment, Captures);
}

/// Builds a new CXXFragmentExpr using the provided reflection, and
/// captures.
///
/// Fragment expressions create a new class, defined approximately,
/// like this:
///
///   struct __fragment_type  {
///     // Reflection of decl to be injected
///     meta::info fragment_reflection;
///
///     // Fragment captures
///     auto __param_<cap_1_identifier_name>;
///     ...
///     auto __param_<cap_n_identifier_name>;
///   };
///
static
CXXFragmentExpr *SynthesizeFragmentExpr(Sema &S,
                                        SourceLocation Loc,
                                        CXXFragmentDecl *FD,
                                        CXXReflectExpr *Reflection,
                                        SmallVectorImpl<Expr *> &Captures) {
  ASTContext &Context = S.Context;
  DeclContext *CurContext = S.CurContext;

  // Build our new class implicit class to hold our fragment info.
  CXXRecordDecl *Class = CXXRecordDecl::Create(Context, TTK_Class, CurContext,
                                               Loc, Loc,
                                               /*Id=*/nullptr,
                                               /*PrevDecl=*/nullptr);
  S.StartDefinition(Class);

  Class->setImplicit(true);
  Class->setFragment(true);

  QualType ClassTy = Context.getRecordType(Class);
  TypeSourceInfo *ClassTSI = Context.getTrivialTypeSourceInfo(ClassTy);


  // Build the class fields.
  SmallVector<FieldDecl *, 4> Fields;

  // Build the field for the reflection itself.
  QualType ReflectionType = Reflection->getType();
  IdentifierInfo *ReflectionFieldId = &Context.Idents.get(
      "fragment_reflection");
  TypeSourceInfo *ReflectionTypeInfo = Context.getTrivialTypeSourceInfo(
      ReflectionType);

  QualType ConstReflectionType = ReflectionType.withConst();
  FieldDecl *Field = FieldDecl::Create(Context, Class, Loc, Loc,
                                       ReflectionFieldId, ConstReflectionType,
                                       ReflectionTypeInfo,
                                       nullptr, false,
                                       ICIS_NoInit);
  Field->setAccess(AS_public);
  Field->setImplicit(true);

  Fields.push_back(Field);
  Class->addDecl(Field);

  // Build the capture fields.
  for (Expr *E : Captures) {
    VarDecl *Var = GetVariableFromCapture(E);
    std::string Name = "__captured_" + Var->getIdentifier()->getName().str();
    IdentifierInfo *Id = &Context.Idents.get(Name);
    TypeSourceInfo *TypeInfo = Context.getTrivialTypeSourceInfo(Var->getType());
    FieldDecl *Field = FieldDecl::Create(Context, Class, Loc, Loc, Id,
                                         Var->getType(), TypeInfo,
                                         nullptr, false,
                                         ICIS_NoInit);
    Field->setAccess(AS_public);
    Field->setImplicit(true);

    Fields.push_back(Field);
    Class->addDecl(Field);
  }

  // Build a new constructor for our fragment type.
  DeclarationName Name = Context.DeclarationNames.getCXXConstructorName(
      Context.getCanonicalType(ClassTy));
  DeclarationNameInfo NameInfo(Name, Loc);
  CXXConstructorDecl *Ctor = CXXConstructorDecl::Create(
      Context, Class, Loc, NameInfo, /*Type*/QualType(), /*TInfo=*/nullptr,
      /*isExplicit=*/true, /*isInline=*/true, /*isImplicitlyDeclared=*/false,
      /*isConstexpr=*/true);
  Ctor->setAccess(AS_public);

  // Build the function type for said constructor.
  FunctionProtoType::ExtProtoInfo EPI;
  EPI.ExceptionSpec.Type = EST_Unevaluated;
  EPI.ExceptionSpec.SourceDecl = Ctor;
  EPI.ExtInfo = EPI.ExtInfo.withCallingConv(
      Context.getDefaultCallingConvention(/*IsVariadic=*/false,
                                          /*IsCXXMethod=*/true));

  SmallVector<QualType, 4> ArgTypes;
  ArgTypes.push_back(ReflectionType);
  for (Expr *E : Captures)
    ArgTypes.push_back(E->getType());

  QualType CtorTy = Context.getFunctionType(Context.VoidTy, ArgTypes, EPI);
  Ctor->setType(CtorTy);

  // Build the constructor params.
  SmallVector<ParmVarDecl *, 4> Parms;

  // Build the constructor param for the reflection param.
  IdentifierInfo *ReflectionParmId = &Context.Idents.get("fragment_reflection");
  ParmVarDecl *Parm = ParmVarDecl::Create(Context, Ctor, Loc, Loc,
                                          ReflectionParmId,
                                          ReflectionType, ReflectionTypeInfo,
                                          SC_None, nullptr);
  Parm->setScopeInfo(0, 0);
  Parm->setImplicit(true);
  Parms.push_back(Parm);

  // Build the constructor capture params.
  for (std::size_t I = 0; I < Captures.size(); ++I) {
    Expr *E = Captures[I];
    VarDecl *Var = GetVariableFromCapture(E);
    std::string Name = "__param_" + Var->getIdentifier()->getName().str();
    IdentifierInfo *Id = &Context.Idents.get(Name);
    QualType ParmTy = E->getType();
    TypeSourceInfo *TypeInfo = Context.getTrivialTypeSourceInfo(ParmTy);
    ParmVarDecl *Parm = ParmVarDecl::Create(Context, Ctor, Loc, Loc,
                                            Id, ParmTy, TypeInfo,
                                            SC_None, nullptr);
    Parm->setScopeInfo(0, I + 1);
    Parm->setImplicit(true);
    Parms.push_back(Parm);
  }

  Ctor->setParams(Parms);

  // Build constructor initializers.
  std::size_t NumInits = Fields.size();
  CXXCtorInitializer **Inits = new (Context) CXXCtorInitializer *[NumInits];

  // Build member initializers.
  for (std::size_t I = 0; I < Parms.size(); ++I) {
    ParmVarDecl *Parm = Parms[I];
    FieldDecl *Field = Fields[I];
    DeclRefExpr *Ref = new (Context) DeclRefExpr(
        Parm, false, Parm->getType(), VK_LValue, Loc);
    Expr *Arg = new (Context) ParenListExpr(Context, Loc, Ref, Loc);
    Inits[I] = S.BuildMemberInitializer(Field, Arg, Loc).get();
  }
  Ctor->setNumCtorInitializers(NumInits);
  Ctor->setCtorInitializers(Inits);

  // Build the definition.
  Stmt *Def = CompoundStmt::Create(Context, None, Loc, Loc);
  Ctor->setBody(Def);
  Class->addDecl(Ctor);

  S.CompleteDefinition(Class);

  // Setup the arguments to use for initialization.
  SmallVector<Expr *, 8> CtorArgs;
  CtorArgs.push_back(dyn_cast<Expr>(Reflection));
  for (Expr *E : Captures) {
    CtorArgs.push_back(E);
  }

  // Build an expression that that initializes the fragment object.
  Expr *Init;
  if (CtorArgs.size() == 1) {
    CXXConstructExpr *Cast = CXXConstructExpr::Create(
        Context, ClassTy, Loc, Ctor, true, CtorArgs,
        /*HadMultipleCandidates=*/false, /*ListInitialization=*/false,
        /*StdInitListInitialization=*/false, /*ZeroInitialization=*/false,
        CXXConstructExpr::CK_Complete, SourceRange(Loc, Loc));
    Init = CXXFunctionalCastExpr::Create(
        Context, ClassTy, VK_RValue, ClassTSI, CK_NoOp, Cast,
        /*Path=*/nullptr, Loc, Loc);
  } else {
    Init = new (Context) CXXTemporaryObjectExpr(
        Context, Ctor, ClassTy, ClassTSI, CtorArgs, SourceRange(Loc, Loc),
        /*HadMultipleCandidates=*/false, /*ListInitialization=*/false,
        /*StdInitListInitialization=*/false, /*ZeroInitialization=*/false);
  }

  // Finally, build the fragment expression.
  return new (Context) CXXFragmentExpr(Context, Loc, ClassTy, FD, Captures,
                                       Init);
}

/// Builds a new fragment expression.
ExprResult Sema::BuildCXXFragmentExpr(SourceLocation Loc, Decl *Fragment,
                                      SmallVectorImpl<Expr *> &Captures) {
  CXXFragmentDecl *FD = cast<CXXFragmentDecl>(Fragment);

  // If the fragment appears in a context that depends on template parameters,
  // then the expression is dependent.
  //
  // FIXME: This is just an approximation of the right answer. In truth, the
  // expression is dependent if the fragment depends on any template parameter
  // in this or any enclosing context.
  if (CurContext->isDependentContext()) {
    return new (Context) CXXFragmentExpr(Context, Loc, Context.DependentTy,
                                         FD, Captures, nullptr);
  }

  // Build the expression used to the reflection of fragment.
  ExprResult Reflection = BuildCXXReflectExpr(/*Loc*/SourceLocation(),
                                              FD->getContent(),
                                              /*LP=*/SourceLocation(),
                                              /*RP=*/SourceLocation());
  if (Reflection.isInvalid())
    return ExprError();

  return SynthesizeFragmentExpr(
      *this, Loc, FD, static_cast<CXXReflectExpr *>(Reflection.get()),
      Captures);
}

/// Returns an injection statement.
StmtResult Sema::ActOnCXXInjectionStmt(SourceLocation Loc,
                                       Expr *FragmentOrReflection) {
  return BuildCXXInjectionStmt(Loc, FragmentOrReflection);
}

static bool
isTypeOrValueDependent(Expr *FragmentOrReflection) {
  return FragmentOrReflection->isTypeDependent()
      || FragmentOrReflection->isValueDependent();
}

static bool
isFragmentType(QualType Type) {
  if (CXXRecordDecl *RD = Type->getAsCXXRecordDecl())
    return RD->isFragment();
  return false;
}

static bool
CheckInjectionOperand(Sema &S, Expr *Operand) {
  QualType Type = Operand->getType();
  if (isFragmentType(Type))
    return true;

  if (Type->isReflectionType())
    return true;

  S.Diag(Operand->getExprLoc(), diag::err_invalid_injection_operand)
    << Type;
  return false;
}

/// Returns an injection statement.
StmtResult Sema::BuildCXXInjectionStmt(SourceLocation Loc, Expr *Operand) {
  // If the operand is not dependent, it must be resolveable either
  // to an injectable reflection, or a fragment.
  //
  // An injectable reflection is defined as any reflection which
  // we can resolve to a declaration.
  bool IsDependent = isTypeOrValueDependent(Operand);
  if (!IsDependent && !CheckInjectionOperand(*this, Operand)) {
    return StmtError();
  }

  // Perform an lvalue-to-value conversion so that we get an rvalue in
  // evaluation.
  if (Operand->isGLValue())
    Operand = ImplicitCastExpr::Create(Context, Operand->getType(),
                                       CK_LValueToRValue, Operand,
                                       nullptr, VK_RValue);

  return new (Context) CXXInjectionStmt(Loc, Operand);
}

// Returns an integer value describing the target context of the injection.
// This correlates to the second %select in err_invalid_injection.
static int DescribeInjectionTarget(DeclContext *DC) {
  if (DC->isFunctionOrMethod())
    return 0;
  else if (DC->isRecord())
    return 1;
  else if (DC->isNamespace())
    return 2;
  else if (DC->isTranslationUnit())
    return 3;
  else
    llvm_unreachable("Invalid injection context");
}

struct TypedValue
{
  QualType Type;
  APValue Value;
};

// Generate an error injecting a declaration of kind SK into the given
// declaration context. Returns false. Note that SK correlates to the first
// %select in err_invalid_injection.
static bool InvalidInjection(Sema& S, SourceLocation POI, int SK,
                             DeclContext *DC) {
  S.Diag(POI, diag::err_invalid_injection) << SK << DescribeInjectionTarget(DC);
  return false;
}

static bool CheckInjectionContexts(Sema &SemaRef, SourceLocation POI,
                                   DeclContext *Injection,
                                   DeclContext *Injectee) {
  if (Injection->isRecord() && !Injectee->isRecord()) {
    InvalidInjection(SemaRef, POI, 1, Injectee);
    return false;
  } else if (Injection->isFileContext() && !Injectee->isFileContext()) {
    InvalidInjection(SemaRef, POI, 0, Injectee);
    return false;
  }
  return true;
}

template<typename F>
static bool BootstrapInjection(Sema &S, Decl *Injectee, F InjectionProcedure) {
  // Create an injection context and then execute the logic for that
  // context.
  InjectionContext *Ctx = new InjectionContext(S);
  InjectionProcedure(Ctx);

  // If we're injecting into a class and have pending definitions, attach
  // those to the class for subsequent analysis.
  if (CXXRecordDecl *ClassInjectee = dyn_cast<CXXRecordDecl>(Injectee)) {
    if (!Injectee->isInvalidDecl() && !Ctx->InjectedDefinitions.empty()) {
      S.PendingClassMemberInjections.push_back(Ctx->Detach());
      return true;
    }
  }

  delete Ctx;
  return !Injectee->isInvalidDecl();
}

/// Inject a fragment into the current context.
static bool InjectFragment(Sema &S,
                           SourceLocation POI,
                           Decl *Injection,
                           const SmallVector<InjectionCapture, 8> &Captures,
                           Decl *Injectee) {
  DeclContext *InjectionDC = Decl::castToDeclContext(Injection);
  DeclContext *InjecteeDC = Decl::castToDeclContext(Injectee);

  if (!CheckInjectionContexts(S, POI, InjectionDC, InjecteeDC))
    return false;

  Sema::ContextRAII Switch(S, InjecteeDC, isa<CXXRecordDecl>(Injectee));

  return BootstrapInjection(S, Injectee, [&](InjectionContext *Ctx) {
    // Setup substitutions
    Ctx->AddDeclSubstitution(Injection, Injectee);
    Ctx->AddPlaceholderSubstitutions(Injection->getDeclContext(), Captures);

    // Inject each declaration in the fragment.
    for (Decl *D : InjectionDC->decls()) {
      // Never inject injected class names.
      if (CXXRecordDecl *Class = dyn_cast<CXXRecordDecl>(D))
        if (Class->isInjectedClassName())
          continue;

      Decl *R = Ctx->InjectDecl(D);
      if (!R || R->isInvalidDecl()) {
        Injectee->setInvalidDecl(true);
        continue;
      }
    }
  });
}

// Inject a reflected declaration into the current context.
static bool CopyDeclaration(Sema &S, SourceLocation POI,
                            Decl *Injection,
                            const ReflectionModifiers &Modifiers,
                            Decl *Injectee) {
  DeclContext *InjectionDC = Injection->getDeclContext();
  Decl *InjectionOwner = Decl::castFromDeclContext(InjectionDC);
  DeclContext *InjecteeDC = Decl::castToDeclContext(Injectee);

  // Don't copy injected class names.
  if (CXXRecordDecl *Class = dyn_cast<CXXRecordDecl>(Injection))
    if (Class->isInjectedClassName())
      return true;

  if (!CheckInjectionContexts(S, POI, InjectionDC, InjecteeDC))
    return false;

  // Establish injectee as the current context.
  Sema::ContextRAII Switch(S, InjecteeDC, isa<CXXRecordDecl>(Injectee));

  return BootstrapInjection(S, Injectee, [&](InjectionContext *Ctx) {
    // Setup substitutions
    Ctx->AddDeclSubstitution(InjectionOwner, Injectee);

    Ctx->SetModifiers(Modifiers);

    // Inject the declaration.
    Decl* Result = Ctx->InjectDecl(Injection);
    if (!Result || Result->isInvalidDecl()) {
      Injectee->setInvalidDecl(true);
    }
  });
}

static Reflection
GetReflectionFromFrag(Sema &S, InjectionEffect &IE) {
  const int REFLECTION_INDEX = 0;

  APValue FragmentData = IE.ExprValue;
  APValue APRefl = FragmentData.getStructField(REFLECTION_INDEX);

  return Reflection(S.Context, APRefl);
}

static const Decl *
GetFragInjectionDecl(Sema &S, InjectionEffect &IE) {
  Reflection &&Refl = GetReflectionFromFrag(S, IE);
  const Decl *Decl = Refl.getAsDeclaration();

  // Verify that our fragment contained a declaration.
  assert(Decl);
  return Decl;
}

static SmallVector<InjectionCapture, 8>
GetFragCaptures(InjectionEffect &IE) {
  // Setup field decls for iteration.
  CXXRecordDecl *FragmentClosureDecl = IE.ExprType->getAsCXXRecordDecl();
  assert(FragmentClosureDecl);
  auto DeclIterator    = FragmentClosureDecl->field_begin();
  auto DeclIteratorEnd = FragmentClosureDecl->field_end();

  // Setup field values for iteration.
  APValue FragmentData = IE.ExprValue;

  // Do not include the first field, it's used to store the
  // reflection not a capture.
  unsigned NumCaptures = FragmentData.getStructNumFields() - 1;
  DeclIterator++;

  // Allocate space in advanced as we know the size.
  SmallVector<InjectionCapture, 8> Captures;
  Captures.reserve(NumCaptures);

  // Map the capture decls to their values.
  for (unsigned int I = 0; I < NumCaptures; ++I) {
    assert(DeclIterator != DeclIteratorEnd);

    auto &&CaptureDecl = *DeclIterator++;
    auto &&CaptureValue = FragmentData.getStructField(I + 1);

    Captures.emplace_back(CaptureDecl, CaptureValue);
  }

  // Verify that all captures were copied,
  // and by association, that the number of
  // values matches the number of decls.
  assert(DeclIterator == DeclIteratorEnd);
  return Captures;
}

static bool ApplyFragmentInjection(Sema &S, SourceLocation POI,
                                   InjectionEffect &IE, Decl *Injectee) {
  Decl *Injection = const_cast<Decl *>(GetFragInjectionDecl(S, IE));
  SmallVector<InjectionCapture, 8> &&Captures = GetFragCaptures(IE);
  return InjectFragment(S, POI, Injection, Captures, Injectee);
}

static Reflection
GetReflectionFromInjection(Sema &S, InjectionEffect &IE) {
  return Reflection(S.Context, IE.ExprValue);
}

static Decl *
GetInjectionDecl(const Reflection &Refl) {
  const Decl *ReachableDecl = Refl.getAsReachableDeclaration();

  // Verify that our reflection contains a reachable declaration.
  assert(ReachableDecl);
  return const_cast<Decl *>(ReachableDecl);
}

static const ReflectionModifiers &
GetModifiers(const Reflection &Refl) {
  return Refl.getModifiers();
}

static bool ApplyReflectionInjection(Sema &S, SourceLocation POI,
                                     InjectionEffect &IE, Decl *Injectee) {
  Reflection &&Refl = GetReflectionFromInjection(S, IE);

  Decl *Injection = GetInjectionDecl(Refl);
  ReflectionModifiers Modifiers = GetModifiers(Refl);

  return CopyDeclaration(S, POI, Injection, Modifiers, Injectee);
}

bool Sema::ApplyInjection(SourceLocation POI, InjectionEffect &IE) {
  Decl *Injectee = Decl::castFromDeclContext(CurContext);
  if (!Injectee)
    return false;

  // FIXME: We need to validate the Injection is compatible
  // with the Injectee.

  if (isFragmentType(IE.ExprType)) {
    return ApplyFragmentInjection(*this, POI, IE, Injectee);
  }

  // Type checking should gauarantee that the type of
  // our injection is either a Fragment or reflection.
  // Since we failed the fragment check, we must have
  // a reflection.
  assert(IE.ExprType->isReflectionType());

  return ApplyReflectionInjection(*this, POI, IE, Injectee);
}

/// Inject a sequence of source code fragments or modification requests
/// into the current AST. The point of injection (POI) is the point at
/// which the injection is applied.
///
/// returns true if no errors are encountered, false otherwise.
bool Sema::ApplyEffects(SourceLocation POI,
                        SmallVectorImpl<InjectionEffect> &Effects) {
  bool Ok = true;
  for (InjectionEffect &Effect : Effects) {
    Ok &= ApplyInjection(POI, Effect);
  }
  return Ok;
}


/// Check if there are any pending definitions of member functions for
/// this class or any of its nested class definitions. We can simply look
/// at the most recent injection; if it's D or declared inside D, then
/// the answer is yes. Otherwise the answer is no.
///
/// We need to check for this whenever a class is completed during an
/// injection. We don't want to prematurely inject definitions.
///
/// FIXME: It's likely that this wouldn't be necessarily if we integrated
/// injection contexts into the template instantiation context; they are
/// somewhat similar.
bool Sema::HasPendingInjections(DeclContext *D) {
  bool IsEmpty = PendingClassMemberInjections.empty();
  if (IsEmpty)
    return false;
  InjectionContext *Cxt = PendingClassMemberInjections.back();
  assert(!Cxt->InjectedDefinitions.empty() && "bad injection queue");
  InjectedDef& Def = Cxt->InjectedDefinitions.front();
  DeclContext *DC = Def.Injected->getDeclContext();
  while (!DC->isFileContext()) {
    if (DC == D)
      return true;
    DC = DC->getParent();
  }
  return false;
}

static void CleanupUsedContexts(
  std::deque<InjectionContext *>& PendingClassMemberInjections) {
  while (!PendingClassMemberInjections.empty()) {
    delete PendingClassMemberInjections.back();
    PendingClassMemberInjections.pop_back();
  }
}

void Sema::InjectPendingFieldDefinitions() {
  for (auto&& Cxt : PendingClassMemberInjections) {
    InjectPendingFieldDefinitions(Cxt);
  }
}

void Sema::InjectPendingMethodDefinitions() {
  for (auto&& Cxt : PendingClassMemberInjections) {
    InjectPendingMethodDefinitions(Cxt);
  }
  CleanupUsedContexts(PendingClassMemberInjections);
}

void Sema::InjectPendingFieldDefinitions(InjectionContext *Cxt) {
  Cxt->Attach();
  for (InjectedDef& Def : Cxt->InjectedDefinitions) {
    if (Def.Type != InjectedDef_Field)
      continue;

    InjectPendingDefinition(Cxt,
                            static_cast<FieldDecl *>(Def.Fragment),
                            static_cast<FieldDecl *>(Def.Injected));
  }
}

void Sema::InjectPendingMethodDefinitions(InjectionContext *Cxt) {
  Cxt->Attach();
  for (InjectedDef& Def : Cxt->InjectedDefinitions) {
    if (Def.Type != InjectedDef_Method)
      continue;

    InjectPendingDefinition(Cxt,
                            static_cast<CXXMethodDecl *>(Def.Fragment),
                            static_cast<CXXMethodDecl *>(Def.Injected));
  }
}

void Sema::InjectPendingDefinition(InjectionContext *Cxt,
                                   FieldDecl *OldField,
                                   FieldDecl *NewField) {
  // Switch to the class enclosing the newly injected declaration.
  ContextRAII ClassCxt (*this, NewField->getDeclContext());

  // This is necessary to provide the correct lookup behavior
  // for any injected field with a default initializer using
  // a decl owned by the injectee
  this->CXXThisTypeOverride = Context.getPointerType(
    Context.getRecordType(NewField->getParent()));

  ExprResult Init = Cxt->TransformExpr(OldField->getInClassInitializer());
  if (Init.isInvalid())
    NewField->setInvalidDecl();
  else
    NewField->setInClassInitializer(Init.get());
}

void Sema::InjectPendingDefinition(InjectionContext *Cxt,
                                   CXXMethodDecl *OldMethod,
                                   CXXMethodDecl *NewMethod) {
  SynthesizedFunctionScope Scope(*this, NewMethod);

  ContextRAII MethodCxt (*this, NewMethod);
  StmtResult Body = Cxt->TransformStmt(OldMethod->getBody());
  if (Body.isInvalid())
    NewMethod->setInvalidDecl();
  else
    NewMethod->setBody(Body.get());

  if (CXXConstructorDecl *OldCtor = dyn_cast<CXXConstructorDecl>(OldMethod)) {
    CXXConstructorDecl *NewCtor = cast<CXXConstructorDecl>(NewMethod);

    int NumCtorInits = OldCtor->getNumCtorInitializers();
    CXXCtorInitializer** NewCtorMem = new CXXCtorInitializer*[NumCtorInits]();

    MutableArrayRef<CXXCtorInitializer *> NewInitArgs(NewCtorMem, NumCtorInits);

    auto OldIterator = OldCtor->init_begin();
    auto OldIteratorEnd = OldCtor->init_end();
    auto NewIterator = NewInitArgs.begin();
    auto NewIteratorEnd = NewInitArgs.end();

    while (OldIterator != OldIteratorEnd && NewIterator != NewIteratorEnd) {
      CXXCtorInitializer* OldInitializer = *OldIterator;

      ASTContext &AST = getASTContext();
      FieldDecl* NewField = cast<FieldDecl>(
        Cxt->GetDeclReplacement(OldInitializer->getMember()));
      ExprResult NewInit = Cxt->TransformExpr(OldInitializer->getInit());

      // TODO: this assumes a member initializer
      // there are other ctor initializer types we need to
      // handle
      CXXCtorInitializer* NewInitializer = new CXXCtorInitializer(
        AST, NewField, OldInitializer->getMemberLocation(),
        OldInitializer->getLParenLoc(), NewInit.get(),
        OldInitializer->getRParenLoc());

      *NewIterator = NewInitializer;

      OldIterator++;
      NewIterator++;
    }

    SetCtorInitializers(NewCtor, /*AnyErrors=*/false, NewInitArgs);
    // FIXME: We should run diagnostics here
    // DiagnoseUninitializedFields(*this, Constructor);
  }
}


/// Returns true if a constexpr-declaration in declaration context DC
/// would be represented using a function (vs. a lambda).
static inline bool NeedsFunctionRepresentation(const DeclContext *DC) {
  return DC->isFileContext() || DC->isRecord();
}


template <typename MetaType>
static MetaType *
ActOnMetaDecl(Sema &Sema, Scope *S, SourceLocation ConstexprLoc,
              unsigned &ScopeFlags) {

  Preprocessor &PP = Sema.PP;
  ASTContext &Context = Sema.Context;
  DeclContext *&CurContext = Sema.CurContext;

  MetaType *MD;
  if (NeedsFunctionRepresentation(CurContext)) {
    ScopeFlags = Scope::FnScope | Scope::DeclScope;
    Sema.PushFunctionScope();

    // Build the function
    //
    //  constexpr void __constexpr_decl() compound-statement
    //
    // where compound-statement is the as-of-yet parsed body of the
    // constexpr-declaration.
    IdentifierInfo *II = &PP.getIdentifierTable().get("__constexpr_decl");
    DeclarationName Name(II);
    DeclarationNameInfo NameInfo(Name, ConstexprLoc);

    FunctionProtoType::ExtProtoInfo EPI(
        Context.getDefaultCallingConvention(/*IsVariadic=*/false,
                                            /*IsCXXMethod=*/false));
    QualType FunctionTy = Context.getFunctionType(Context.VoidTy, None, EPI);
    TypeSourceInfo *FunctionTyInfo =
        Context.getTrivialTypeSourceInfo(FunctionTy);

    // FIXME: Why is the owner the current context? We should probably adjust
    // this to the constexpr-decl later on. Maybe the owner should be the
    // nearest file context, since this is essentially a non-member function.
    FunctionDecl *Function =
        FunctionDecl::Create(Context, CurContext, ConstexprLoc, NameInfo,
                             FunctionTy, FunctionTyInfo, SC_None,
                             /*isInlineSpecified=*/false,
                             /*hasWrittenPrototype=*/true,
                             /*isConstexprSpecified=*/true);
    Function->setImplicit();
    Function->setMetaprogram();

    // Build the meta declaration around the function.
    MD = MetaType::Create(Context, CurContext, ConstexprLoc, Function);
  } else if (CurContext->isFunctionOrMethod()) {
    ScopeFlags = Scope::BlockScope | Scope::FnScope | Scope::DeclScope;

    LambdaScopeInfo *LSI = Sema.PushLambdaScope();

    // Build the expression
    //
    //    []() -> void compound-statement
    //
    // where compound-statement is the as-of-yet parsed body of the
    // constexpr-declaration. Note that the return type is not deduced (it
    // doesn't need to be).
    //
    // TODO: It would be great if we could only capture constexpr declarations,
    // but C++ doesn't have a constexpr default.
    const bool KnownDependent = S->getTemplateParamParent();

    FunctionProtoType::ExtProtoInfo EPI(
        Context.getDefaultCallingConvention(/*IsVariadic=*/false,
                                            /*IsCXXMethod=*/true));
    EPI.HasTrailingReturn = true;
    EPI.TypeQuals |= DeclSpec::TQ_const;
    QualType MethodTy = Context.getFunctionType(Context.VoidTy, None, EPI);
    TypeSourceInfo *MethodTyInfo = Context.getTrivialTypeSourceInfo(MethodTy);

    LambdaIntroducer Intro;
    Intro.Range = SourceRange(ConstexprLoc);
    Intro.Default = LCD_None;

    CXXRecordDecl *Closure = Sema.createLambdaClosureType(
        Intro.Range, MethodTyInfo, KnownDependent, Intro.Default);
    CXXMethodDecl *Method =
        Sema.startLambdaDefinition(Closure, Intro.Range, MethodTyInfo,
                                   ConstexprLoc, None,
                                   /*IsConstexprSpecified=*/true);
    Sema.buildLambdaScope(LSI, Method, Intro.Range, Intro.Default,
                          Intro.DefaultLoc,
                          /*ExplicitParams=*/false,
                          /*ExplicitResultType=*/true,
                          /*Mutable=*/false);
    Method->setMetaprogram();

    // NOTE: The call operator is not yet attached to the closure type. That
    // happens in ActOnFinishCXXMetaprogramDecl(). The operator is, however,
    // available in the LSI.
    MD = MetaType::Create(Context, CurContext, ConstexprLoc, Closure);
  } else
    llvm_unreachable("constexpr declaration in unsupported context");

  // Add the declaration to the current context. This will be removed from the
  // AST after evaluation.
  CurContext->addDecl(MD);

  return MD;
}
/// Create a metaprogram-declaration that will hold the body of the
/// metaprogram-declaration.
///
/// \p ScopeFlags is set to the value that should be used to create the scope
/// containing the metaprogram-declaration body.
Decl *Sema::ActOnCXXMetaprogramDecl(Scope *S, SourceLocation ConstexprLoc,
                                    unsigned &ScopeFlags) {
  return ActOnMetaDecl<CXXMetaprogramDecl>(*this, S, ConstexprLoc, ScopeFlags);
}

/// Create a injection-declaration that will hold the body of the
/// injection-declaration.
///
/// \p ScopeFlags is set to the value that should be used to create the scope
/// containing the injection-declaration body.
Decl *Sema::ActOnCXXInjectionDecl(Scope *S, SourceLocation ConstexprLoc,
                                  unsigned &ScopeFlags) {
  return ActOnMetaDecl<CXXInjectionDecl>(*this, S, ConstexprLoc, ScopeFlags);
}

template <typename MetaType>
static void
ActOnStartMetaDecl(Sema &Sema, Scope *S, Decl *D) {
  MetaType *MD = cast<MetaType>(D);

  if (MD->hasFunctionRepresentation()) {
    if (S)
      Sema.PushDeclContext(S, MD->getFunctionDecl());
    else
      Sema.CurContext = MD->getFunctionDecl();
  } else {
    LambdaScopeInfo *LSI = cast<LambdaScopeInfo>(Sema.FunctionScopes.back());

    if (S)
      Sema.PushDeclContext(S, LSI->CallOperator);
    else
      Sema.CurContext = LSI->CallOperator;

    Sema.PushExpressionEvaluationContext(
        Sema::ExpressionEvaluationContext::PotentiallyEvaluated);
  }
}

/// Called just prior to parsing the body of a metaprogram-declaration.
///
/// This ensures that the declaration context is pushed with the appropriate
/// scope.
void Sema::ActOnStartCXXMetaprogramDecl(Scope *S, Decl *D) {
  ActOnStartMetaDecl<CXXMetaprogramDecl>(*this, S, D);
}

/// Called just prior to parsing the body of a injection-declaration.
///
/// This ensures that the declaration context is pushed with the appropriate
/// scope.
void Sema::ActOnStartCXXInjectionDecl(Scope *S, Decl *D) {
  ActOnStartMetaDecl<CXXInjectionDecl>(*this, S, D);
}

template <typename MetaType>
static void
DoneWithMetaprogram(MetaType *MD) {
  // Remove the declaration; we don't want to see it in the source tree.
  //
  // FIXME: Do we really want to do this?
  MD->getDeclContext()->removeDecl(MD);
}

/// Evaluate the expression.
///
/// \returns  \c true if the expression \p E can be evaluated, \c false
///           otherwise.
///
template <typename MetaType>
static bool
EvaluateMetaDeclCall(Sema &Sema, MetaType *MD, CallExpr *Call) {
  const LangOptions &LangOpts = Sema.LangOpts;
  ASTContext &Context = Sema.Context;

  // Associate the call expression with the declaration.
  MD->setCallExpr(Call);

  SmallVector<PartialDiagnosticAt, 8> Notes;
  SmallVector<InjectionEffect, 16> Effects;
  Expr::EvalResult Result;
  Result.Diag = &Notes;
  Result.InjectionEffects = &Effects;

  bool Folded = Call->EvaluateAsRValue(Result, Context);
  if (!Folded) {
    // If the only error is that we didn't initialize a (void) value, that's
    // actually okay. APValue doesn't know how to do this anyway.
    //
    // FIXME: We should probably have a top-level EvaluateAsVoid() function that
    // handles this case.
    if (!Notes.empty()) {
      // If we got a compiler error, then just emit that.
      if (Notes[0].second.getDiagID() == diag::err_user_defined_error)
        Sema.Diag(MD->getBeginLoc(), Notes[0].second);
      else if (Notes[0].second.getDiagID() != diag::note_constexpr_uninitialized) {
        // FIXME: These source locations are wrong.
        Sema.Diag(MD->getBeginLoc(), diag::err_expr_not_ice) << LangOpts.CPlusPlus;
        for (const PartialDiagnosticAt &Note : Notes)
          Sema.Diag(Note.first, Note.second);
      }
    }
  }

  // Apply any modifications, and if successful, remove the declaration from
  // the class; it shouldn't be visible in the output code.
  SourceLocation POI = MD->getSourceRange().getEnd();
  Sema.ApplyEffects(POI, Effects);

  DoneWithMetaprogram(MD);

  return Notes.empty();
}

/// Process a metaprogram decl
///
/// This builds an unnamed constexpr void function whose body is that of
/// the metaprogram delaration, and evaluates a call to that function.
template <typename MetaType>
static bool
EvaluateMetaDecl(Sema &Sema, MetaType *MD, FunctionDecl *D) {
  ASTContext &Context = Sema.Context;

  QualType FunctionTy = D->getType();
  DeclRefExpr *Ref =
      new (Context) DeclRefExpr(D, /*RefersToEnclosingVariableOrCapture=*/false,
                                FunctionTy, VK_LValue, SourceLocation());
  QualType PtrTy = Context.getPointerType(FunctionTy);
  ImplicitCastExpr *Cast =
      ImplicitCastExpr::Create(Context, PtrTy, CK_FunctionToPointerDecay, Ref,
                               /*BasePath=*/nullptr, VK_RValue);
  CallExpr *Call =
      new (Context) CallExpr(Context, Cast, ArrayRef<Expr *>(), Context.VoidTy,
                             VK_RValue, SourceLocation());
  return EvaluateMetaDeclCall(Sema, MD, Call);
}

/// Process a metaprogram-declaration.
///
/// This builds an unnamed \c constexpr \c void function whose body is that of
/// the metaprogram delaration, and evaluates a call to that function.
template <typename MetaType>
static bool
EvaluateMetaDecl(Sema &Sema, MetaType *MD, Expr *E) {
  ASTContext &Context = Sema.Context;

  LambdaExpr *Lambda = cast<LambdaExpr>(E);
  CXXMethodDecl *Method = Lambda->getCallOperator();
  QualType MethodTy = Method->getType();
  DeclRefExpr *Ref = new (Context)
      DeclRefExpr(Method, /*RefersToEnclosingVariableOrCapture=*/false,
                  MethodTy, VK_LValue, SourceLocation());
  QualType PtrTy = Context.getPointerType(MethodTy);
  ImplicitCastExpr *Cast =
      ImplicitCastExpr::Create(Context, PtrTy, CK_FunctionToPointerDecay, Ref,
                               /*BasePath=*/nullptr, VK_RValue);
  CallExpr *Call = new (Context) CXXOperatorCallExpr(Context, OO_Call,
                                                     Cast, {Lambda},
                                                     Context.VoidTy,
                                                     VK_RValue,
                                                     SourceLocation(),
                                                     FPOptions());
  return EvaluateMetaDeclCall(Sema, MD, Call);
}

/// Hook to be called by template instantiation.
void Sema::EvaluateCXXMetaDecl(CXXMetaprogramDecl *D, FunctionDecl *FD) {
  EvaluateMetaDecl(*this, D, FD);
}


/// Hook to be called by template instantiation.
void Sema::EvaluateCXXMetaDecl(CXXInjectionDecl *D, FunctionDecl *FD) {
  EvaluateMetaDecl(*this, D, FD);
}

template <typename MetaType>
static void
ActOnFinishMetaDecl(Sema &Sema, Scope *S, Decl *D, Stmt *Body) {
  MetaType *MD = cast<MetaType>(D);
  if (MD->hasFunctionRepresentation()) {
    FunctionDecl *Fn = MD->getFunctionDecl();
    Sema.DiscardCleanupsInEvaluationContext();
    Sema.ActOnFinishFunctionBody(Fn, Body);
    if (!Sema.CurContext->isDependentContext())
      EvaluateMetaDecl(Sema, MD, Fn);
  } else {
    ExprResult Lambda = Sema.ActOnLambdaExpr(MD->getLocation(), Body, S);
    if (!Sema.CurContext->isDependentContext())
      EvaluateMetaDecl(Sema, MD, Lambda.get());
  }

  // If we didn't have a scope when building this, we need to restore the
  // current context.
  if (!S)
    Sema.CurContext = MD->getDeclContext();
}

/// Called immediately after parsing the body of a metaprorgam-declaration.
///
/// The statements within the body are evaluated here.
void Sema::ActOnFinishCXXMetaprogramDecl(Scope *S, Decl *D, Stmt *Body) {
  ActOnFinishMetaDecl<CXXMetaprogramDecl>(*this, S, D, Body);
}

/// Called immediately after parsing the body of a injection-declaration.
///
/// The statements within the body are evaluated here.
void Sema::ActOnFinishCXXInjectionDecl(Scope *S, Decl *D, Stmt *InjectionStmt) {
  CompoundStmt *Body = CompoundStmt::Create(Context,
                                            ArrayRef<Stmt *>(InjectionStmt),
                                            SourceLocation(), SourceLocation());
  ActOnFinishMetaDecl<CXXInjectionDecl>(*this, S, D, Body);
}

template <typename MetaType>
static void
ActOnCXXMetaError(Sema &Sema, Scope *S, Decl *D) {
  MetaType *MD = cast<MetaType>(D);
  MD->setInvalidDecl();
  if (MD->hasFunctionRepresentation())
    Sema.ActOnFinishFunctionBody(MD->getFunctionDecl(), nullptr);
  else
    Sema.ActOnLambdaError(MD->getLocation(), S);

  DoneWithMetaprogram(MD);
}

/// Called when an error occurs while parsing the metaprogram-declaration body.
void Sema::ActOnCXXMetaprogramDeclError(Scope *S, Decl *D) {
  ActOnCXXMetaError<CXXMetaprogramDecl>(*this, S, D);
}

/// Called when an error occurs while parsing the injection-declaration body.
void Sema::ActOnCXXInjectionDeclError(Scope *S, Decl *D) {
  ActOnCXXMetaError<CXXInjectionDecl>(*this, S, D);
}

/// Given an  input like this:
///
///    class(metafn) Proto { ... };
///
/// Generate something that looks (about) like this:
///
///    namespace __fake__ { class Proto { ... } };
///    class Class {
///      using prototype = __fake__::Proto;
///      constexpr { metafn(reflexpr(prototype)); }
///    }
///
/// We don't actually need to emit the fake namespace; we just don't
/// add it to a declaration context.
CXXRecordDecl *Sema::ActOnStartMetaclass(CXXRecordDecl *Class,
                                         Expr *Metafunction, TagUseKind TUK) {
  Class->setMetafunction(Metafunction);

  if (TUK == TUK_Definition) {
    // Start defining the (final) class.
    Class->setLexicalDeclContext(CurContext);
    CurContext->addDecl(Class);
    StartDefinition(Class);

    // Create a new, nested class to hold the parsed member. This must
    // be a fragment in order to suppress default generation of members.
    DeclarationNameInfo DNI(Class->getDeclName(), Class->getLocation());
    CXXRecordDecl *Proto = CXXRecordDecl::Create(
        Context, Class->getTagKind(), Class, Class->getBeginLoc(),
        Class->getLocation(), DNI.getName().getAsIdentifierInfo(), nullptr);

    return Proto;
  }

  return Class;
}

void Sema::ActOnStartMetaclassDefinition(CXXRecordDecl *Proto) {
  Proto->setImplicit(true);
  Proto->setFragment(true);
}

/// We've just finished parsing the definition of something like this:
///
///    class(M) C { ... };
///
/// And have conceptually transformed that into something like this.
///
///    namespace __fake__ { class C { ... } };
///
/// Now, we need to build a new version of the class containing a
/// prototype and its generator.
///
///    class C {
///      using prototype = __fake__::C;
///      constexpr { M(reflexpr(prototype)); }
///    };
///
/// The only remaining step is to build and apply the metaprogram to
/// generate the enclosing class.
CXXRecordDecl *Sema::ActOnFinishMetaclass(CXXRecordDecl *Proto, Scope *S,
                                          SourceRange BraceRange) {
  CXXRecordDecl *Class = cast<CXXRecordDecl>(Proto->getDeclContext());
  Expr *Metafunction = Class->getMetafunction();
  assert(Metafunction && "expected metaclass");

  // FIXME: Are there any properties that Class should inherit from
  // the prototype? Alignment and layout attributes?

  // Propagate access level
  Class->setAccess(Proto->getAccess());

  // Make sure that the final class is available in its declaring scope.
  bool IsAnonymousClass = Class->getName().empty();
  if (!IsAnonymousClass)
    PushOnScopeChains(Class, CurScope->getParent(), false);

  // Make the new class is the current declaration context for the
  // purpose of injecting source code.
  ContextRAII Switch(*this, Class);

  // For the purpose of creating the metaprogram and performing
  // the final analysis, the Class needs to be scope's entity, not
  // prototype.
  S->setEntity(Class);

  // Use the name of the class for most source locations.
  //
  // FIXME: This isn't a particularly good idea.
  SourceLocation Loc = Proto->getLocation();

  // Add 'constexpr { M(reflexpr(prototype)); }' to the class.
  unsigned ScopeFlags;
  Decl *CD = ActOnCXXMetaprogramDecl(CurScope, Loc, ScopeFlags);
  CD->setImplicit(true);
  CD->setAccess(AS_public);

  ActOnStartCXXMetaprogramDecl(CurScope, CD);

  // Build the expression reflexpr(prototype).
  // This technically is performing the equivalent
  // addition of 'constexpr { M(reflexpr(__fake__::C)); }'.
  QualType ProtoTy = Context.getRecordType(Proto);
  ExprResult Input = BuildCXXReflectExpr(/*Loc=*/SourceLocation(),
                                         ProtoTy,
                                         /*LP=*/SourceLocation(),
                                         /*RP=*/SourceLocation());

  // Build the call to <gen>(<ref>)
  Expr *InputExpr = Input.get();
  MultiExprArg Args(InputExpr);
  ExprResult Call = ActOnCallExpr(CurScope, Metafunction, Loc, Args, Loc);
  if (Call.isInvalid()) {
    ActOnCXXMetaprogramDeclError(nullptr, CD);
    Class->setInvalidDecl(true);
  } else {
    Stmt* Body = CompoundStmt::Create(Context, Call.get(), Loc, Loc);
    ActOnFinishCXXMetaprogramDecl(CurScope, CD, Body);

    // Finally, re-analyze the fields of the fields the class to
    // instantiate remaining defaults. This will also complete the
    // definition.
    SmallVector<Decl *, 32> Fields;
    ActOnFields(S, Class->getLocation(), Class, Fields,
                BraceRange.getBegin(), BraceRange.getEnd(),
                ParsedAttributesView());

    ActOnFinishCXXNonNestedClass(Class);

    assert(Class->isCompleteDefinition() && "Generated class not complete");
  }

  return Class;
}
