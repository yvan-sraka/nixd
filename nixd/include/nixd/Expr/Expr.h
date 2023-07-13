/// 'nix::Expr' wrapper that suitable for language server
#pragma once

#include "Nodes.h"

#include <nix/nixexpr.hh>
#include <nix/symbol-table.hh>

namespace nixd {

/// RAII Pool that holds Nodes.
template <class T> class Context {
public:
  std::vector<std::unique_ptr<T>> Nodes;
  template <class U> U *addNode(std::unique_ptr<U> Node) {
    Nodes.push_back(std::move(Node));
    return dynamic_cast<U *>(Nodes.back().get());
  }
  template <class U> U *record(U *Node) {
    Nodes.emplace_back(std::unique_ptr<U>(Node));
    return Node;
  }
};

using ASTContext = Context<nix::Expr>;

template <class Derived> struct RecursiveASTVisitor {

  bool shouldTraversePostOrder() { return false; }

  bool visitExpr(const nix::Expr *) { return true; }

#define NIX_EXPR(EXPR) bool traverse##EXPR(const nix::EXPR *E);
#include "Nodes.inc"
#undef NIX_EXPR

#define NIX_EXPR(EXPR)                                                         \
  bool visit##EXPR(const nix::EXPR *E) { return getDerived().visitExpr(E); }
#include "Nodes.inc"
#undef NIX_EXPR

  bool visitExprError(const nixd::nodes::ExprError *E) {
    return getDerived().visitExpr(E);
  }

  Derived &getDerived() { return *static_cast<Derived *>(this); }

  bool traverseExpr(const nix::Expr *E) {
    if (!E)
      return true;
#define NIX_EXPR(EXPR)                                                         \
  if (auto CE = dynamic_cast<const nix::EXPR *>(E)) {                          \
    return getDerived().traverse##EXPR(CE);                                    \
  }
#include "Nodes.inc"
#undef NIX_EXPR
    if (const auto *CE = dynamic_cast<const nixd::nodes::ExprError *>(E)) {
      return getDerived().traverseExprError(CE);
    }
    assert(false && "We are missing some nix AST Nodes!");
    return true;
  }
  bool traverseExprError(const nixd::nodes::ExprError *);
}; // namespace nixd

#define TRY_TO(CALL_EXPR)                                                      \
  do {                                                                         \
    if (!getDerived().CALL_EXPR)                                               \
      return false;                                                            \
  } while (false)

#define TRY_TO_TRAVERSE(EXPR) TRY_TO(traverseExpr(EXPR))

#define DEF_TRAVERSE_TYPE(TYPE, CODE)                                          \
  template <typename Derived>                                                  \
  bool RecursiveASTVisitor<Derived>::traverse##TYPE(const nix::TYPE *T) {      \
    if (!getDerived().shouldTraversePostOrder())                               \
      TRY_TO(visit##TYPE(T));                                                  \
    { CODE; }                                                                  \
    if (getDerived().shouldTraversePostOrder())                                \
      TRY_TO(visit##TYPE(T));                                                  \
    return true;                                                               \
  }
#include "Traverse.inc"
#undef DEF_TRAVERSE_TYPE
#undef TRY_TO_TRAVERSE

template <typename Derived>
bool RecursiveASTVisitor<Derived>::traverseExprError(
    const nixd::nodes::ExprError *E) {
  if (!getDerived().shouldTraversePostOrder())
    TRY_TO(visitExprError(E));
  if (getDerived().shouldTraversePostOrder())
    TRY_TO(visitExprError(E));
  return true;
}
#undef TRY_TO

inline const char *getExprName(const nix::Expr *E) {
  if (const auto *CE = dynamic_cast<const nixd::nodes::ExprError *>(E)) {
    return "nixd::ExprError";
  }
#define NIX_EXPR(EXPR)                                                         \
  if (dynamic_cast<const nix::EXPR *>(E)) {                                    \
    return #EXPR;                                                              \
  }
#include "Nodes.inc"
  assert(false &&
         "Cannot dynamic-cast to nix::Expr*, missing entries in Nodes.inc?");
  return nullptr;
#undef NIX_EXPR
}

// Traverse on the AST nodes, and construct parent information into the map
[[nodiscard]] std::map<const nix::Expr *, const nix::Expr *>
getParentMap(const nix::Expr *Root);

/// For `ExprVar`s statically look up in `Env`s (i.e. !fromWith), search the
/// position
nix::PosIdx searchDefinition(
    const nix::ExprVar *,
    const std::map<const nix::Expr *, const nix::Expr *> &ParentMap);

const nix::Expr *
searchEnvExpr(const nix::ExprVar *,
              const std::map<const nix::Expr *, const nix::Expr *> &ParentMap);

//-----------------------------------------------------------------------------/
// nix::PosIdx getDisplOf (Expr, Displ)
//
// Get the corresponding position, of the expression Expr setted Displ.
// e.g. ExprLet will create a list of displacement, that denotes to it's
// attributes, used for name lookup.
//
// PosIdx is trivially-copyable

nix::PosIdx getDisplOf(const nix::Expr *E, nix::Displacement Displ);

nix::PosIdx getDisplOf(const nix::ExprAttrs *E, nix::Displacement Displ);

nix::PosIdx getDisplOf(const nix::ExprLet *E, nix::Displacement Displ);

nix::PosIdx getDisplOf(const nix::ExprLambda *E, nix::Displacement Displ);

//-----------------------------------------------------------------------------/
// bool isEnvCreated (Expr* Parent, Expr Child)
//
// Check that if `Parent` created a env for the `Child`.

bool isEnvCreated(const nix::Expr *, const nix::Expr *);

bool isEnvCreated(const nix::ExprAttrs *, const nix::Expr *);

bool isEnvCreated(const nix::ExprWith *, const nix::Expr *);

//-----------------------------------------------------------------------------/

/// Statically collect available symbols in expression's scope.
void collectSymbols(const nix::Expr *, const decltype(getParentMap(nullptr)) &,
                    std::vector<nix::Symbol> &);

} // namespace nixd
