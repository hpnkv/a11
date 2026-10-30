// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef A11_FLOW_DOCUMENT_H_
#define A11_FLOW_DOCUMENT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/flow/module.h"
#include "a11/flow/plan.h"
#include "a11/flow/resolve.h"

namespace a11::flow {

struct CallOptions;

/// A native source expression whose emitter supplies required grouping.
class Expression {
 public:
  static absl::StatusOr<Expression> Reference(std::string name);
  static absl::StatusOr<Expression> Literal(nlohmann::json value);
  static absl::StatusOr<Expression> Invoke(
      std::string action,
      std::vector<std::pair<std::string, Expression>> arguments = {},
      bool remote = false, bool tolerant = false, std::string node_map = {});
  static absl::StatusOr<Expression> InvokeWith(
      std::string action,
      std::vector<std::pair<std::string, Expression>> arguments,
      const CallOptions& options);
  static absl::StatusOr<Expression> Object(
      std::vector<std::pair<std::string, Expression>> fields);
  static absl::StatusOr<Expression> Array(std::vector<Expression> items);
  static absl::StatusOr<Expression> Builtin(std::string name,
                                            std::vector<Expression> arguments);
  static absl::StatusOr<Expression> Node(std::optional<Expression> id = {},
                                         std::string node_map = {});
  static absl::StatusOr<Expression> Completion(std::string operation,
                                               const Expression& subject);
  absl::StatusOr<Expression> Cast(const syntax::TypeExpression& type) const;
  absl::StatusOr<Expression> Index(const Expression& index) const;
  absl::StatusOr<Expression> Field(std::string name) const;
  absl::StatusOr<Expression> Pipeline(
      std::string stage, std::optional<Expression> argument = {}) const;

  [[nodiscard]] const std::string& source() const { return source_; }

 private:
  explicit Expression(std::string source) : source_(std::move(source)) {}

  std::string source_;
};

/// Typed modifiers for an invocation expression.
struct CallOptions {
  bool remote = false;
  bool tolerant = false;
  bool tee = false;
  std::string node_map;
  std::optional<absl::Duration> timeout;
  std::optional<Expression> id;
  std::vector<Expression> after;
  std::vector<std::pair<std::string, Expression>> headers;
  std::vector<std::string> forward;
};

/// Typed statements for constructing executable bodies.
class Statement {
 public:
  static absl::StatusOr<Statement> Let(std::string name,
                                       const Expression& value);
  static absl::StatusOr<Statement> Pipe(const Expression& value,
                                        const Expression& destination);
  static absl::StatusOr<Statement> Wait(const Expression& subject,
                                        bool drain = false);
  static absl::StatusOr<Statement> Cancel(const Expression& subject);
  static absl::StatusOr<Statement> Fail(const Expression& code,
                                        std::optional<Expression> message = {});
  static absl::StatusOr<Statement> If(
      const Expression& condition, const std::vector<Statement>& then_body,
      const std::vector<Statement>& else_body = {});
  static absl::StatusOr<Statement> For(std::string variable,
                                       const Expression& source,
                                       const std::vector<Statement>& body,
                                       int parallel = 1);
  static absl::StatusOr<Statement> After(
      const std::vector<Expression>& subjects,
      const std::vector<Statement>& body, bool tolerant = false);
  static absl::StatusOr<Statement> Block(const std::vector<Statement>& body,
                                         bool tolerant = false);

  const std::string& source() const { return source_; }

 private:
  explicit Statement(std::string source) : source_(std::move(source)) {}

  static absl::StatusOr<Statement> Validated(std::string source);
  static std::string Body(const std::vector<Statement>& statements);
  std::string source_;
};

/// An implemented flow or interface, constructed with native typed helpers.
class FlowBuilder {
 public:
  explicit FlowBuilder(std::string name, bool interface_only = false);

  FlowPlan& interface() { return plan_; }

  const FlowPlan& interface() const { return plan_; }

  absl::Status AddPort(
      std::string name, const syntax::TypeExpression& type,
      syntax::PortDirection direction = syntax::PortDirection::kInput,
      bool unary = true, bool required = false, std::string description = {});
  absl::Status AddStatement(const Statement& statement);
  absl::Status AddLet(std::string name, const Expression& value);
  absl::Status AddPipe(const Expression& value, const Expression& destination);
  absl::Status AddImport(std::string reference, std::string alias,
                         std::string symbol = {});
  absl::Status AddDeclaration(const FlowPlan& interface);
  absl::Status AddStruct(const DtoPlan& shape);
  absl::StatusOr<std::string> Emit() const;

 private:
  FlowPlan plan_;
  std::vector<std::string> statements_;
  std::vector<syntax::Import> imports_;
};

/// Construct a struct using the language's existing shape descriptor.
class StructBuilder {
 public:
  explicit StructBuilder(std::string name) { plan_.name = std::move(name); }

  DtoPlan& shape() { return plan_; }

  const DtoPlan& shape() const { return plan_; }

  absl::Status AddField(std::string name, const syntax::TypeExpression& type,
                        bool required = false,
                        std::optional<Expression> default_value = {},
                        std::string description = {});
  absl::StatusOr<std::string> Emit() const;

 private:
  DtoPlan plan_;
};

/// A symbol's identity in one revision of an owning document.
struct DocumentSymbolHandle {
  std::string document;
  uint64_t revision = 0;
  std::string name;
  std::string kind;
  size_t occurrence = 0;
  size_t start = 0;
  size_t end = 0;
};

/// The source edits committed atomically by one document transaction.
struct DocumentChange {
  uint64_t revision = 0;
  std::vector<Edit> edits;
  std::vector<Diagnostic> diagnostics;
};

/// A validated rename transaction over the supplied source snapshots.
struct ModuleGraphChange {
  ModuleSources sources;
  absl::flat_hash_map<std::string, std::vector<Edit>> edits;
  std::vector<Diagnostic> diagnostics;
};

/// Editable source, separate from immutable linked and executable snapshots.
class ModuleDocument {
 public:
  explicit ModuleDocument(std::string identity = "<memory>",
                          std::string source = {});

  const std::string& identity() const { return identity_; }

  const std::string& source() const { return source_; }

  uint64_t revision() const { return revision_; }

  std::vector<DocumentSymbolHandle> Symbols() const;
  absl::StatusOr<DocumentChange> Apply(uint64_t revision,
                                       std::vector<Edit> edits);
  absl::StatusOr<DocumentChange> Insert(const FlowBuilder& flow);
  absl::StatusOr<DocumentChange> InsertStruct(const DtoPlan& shape);
  absl::StatusOr<DocumentChange> UpsertDeclaration(
      const FlowPlan& interface, const std::vector<DtoPlan>& types = {},
      std::optional<DocumentSymbolHandle> symbol = {});
  absl::StatusOr<DocumentChange> Update(const DocumentSymbolHandle& symbol,
                                        const FlowBuilder& flow);
  absl::StatusOr<DocumentChange> UpdateStruct(
      const DocumentSymbolHandle& symbol, const DtoPlan& shape);
  absl::StatusOr<DocumentChange> Remove(const DocumentSymbolHandle& symbol);
  absl::StatusOr<DocumentChange> SetExported(const DocumentSymbolHandle& symbol,
                                             bool value);
  absl::StatusOr<DocumentChange> UpsertImport(
      std::string reference, std::string alias,
      std::optional<DocumentSymbolHandle> scope = {}, std::string symbol = {});
  absl::StatusOr<DocumentChange> RemoveImport(
      std::string alias, std::optional<DocumentSymbolHandle> scope = {});
  absl::StatusOr<DocumentChange> Rename(const DocumentSymbolHandle& symbol,
                                        std::string name);
  absl::StatusOr<ModuleGraphChange> RenameInGraph(
      const DocumentSymbolHandle& symbol, std::string name,
      const ModuleSources& sources = {}) const;
  absl::StatusOr<DocumentChange> RenameImport(
      std::string alias, std::string name,
      std::optional<DocumentSymbolHandle> scope = {});
  absl::StatusOr<FlowPlan> EffectiveSchema(
      std::string name, const ModuleSources& sources = {}) const;
  LinkedModule Link(const ModuleSources& sources = {},
                    ModuleResolver resolver = {}) const;
  std::vector<Diagnostic> Check(const ModuleSources& sources = {}) const;

 private:
  absl::Status Validate(const DocumentSymbolHandle& symbol) const;
  std::string identity_;
  std::string source_;
  uint64_t revision_ = 0;
};

/// Emit a declaration without introducing an action-layer dependency.
absl::StatusOr<std::string> DeclarationSource(const FlowPlan& plan);
}  // namespace a11::flow
#endif  // A11_FLOW_DOCUMENT_H_
