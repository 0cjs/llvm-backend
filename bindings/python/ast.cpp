#include <kllvm/ast/AST.h>
#include <kllvm/binary/ProofTraceParser.h>
#include <kllvm/binary/deserializer.h>
#include <kllvm/binary/serializer.h>
#include <kllvm/parser/KOREParser.h>

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstddef>
#include <sstream>
#include <tuple>

namespace py = pybind11;

using namespace kllvm;
using namespace kllvm::parser;

// Metaprogramming support for the adapter function between AST print() methods
// and Python's __repr__.
namespace detail {

template <typename T>
struct type_identity {
  using type = T;
};

template <typename T, typename... Args>
struct print_repr_adapter_st {
  print_repr_adapter_st(type_identity<T>, Args &&...args)
      : args_(std::forward<Args>(args)...) { }

  std::string operator()(T &node) {
    auto ss = std::stringstream{};

    std::apply(
        [&](auto &&...args) { return node.print(args...); },
        std::tuple_cat(
            std::tuple{std::ref(ss)}, std::forward<decltype(args_)>(args_)));

    return ss.str();
  }

private:
  std::tuple<Args...> args_;
};

template <typename T, typename... Args>
print_repr_adapter_st(type_identity<T>, Args &&...)
    -> print_repr_adapter_st<T, Args...>;

} // namespace detail

/**
 * Adapt an AST node's print method to return a string for use with Python's
 * __repr__ method.
 */
template <typename T, typename... Args>
auto print_repr_adapter(Args &&...args) {
  return ::detail::print_repr_adapter_st(
      ::detail::type_identity<T>{}, std::forward<Args>(args)...);
}

/**
 * Rather than requiring the entire contents of the binary file to be read into
 * memory ahead of time, this binding uses the optional size field present in
 * version 1.2.0 of the binary format to read only the required portion of the
 * file. If the size is zero, or the input pattern uses an older version, an
 * exception will be thrown. The file pointer will be left at the end of the
 * pattern's bytes after calling this function.
 */
std::shared_ptr<KOREPattern> read_pattern_from_file(py::object &file_like) {
  if (!py::hasattr(file_like, "read")) {
    throw py::type_error("Argument to read_from is not a file-like object");
  }

  auto read_attr = file_like.attr("read");
  auto read = [&read_attr](auto len) -> std::string {
    return py::bytes(read_attr(len));
  };

  auto header = read(5);
  auto ref_header = serializer::magic_header;
  if (!std::equal(header.begin(), header.end(), ref_header.begin())) {
    throw std::invalid_argument(
        "Data does not begin with the binary KORE header bytes");
  }

  auto version_bytes = read(6);
  auto version_begin = version_bytes.begin();
  auto version
      = kllvm::detail::read_version(version_begin, version_bytes.end());

  if (version < binary_version(1, 2, 0)) {
    throw std::invalid_argument(
        "Pattern read from a file-like object must use version 1.2.0 or newer");
  }

  auto size_bytes = read(8);
  auto size_begin = size_bytes.begin();
  auto size = kllvm::detail::read_pattern_size_unchecked(
      size_begin, size_bytes.end());

  if (size == 0) {
    throw std::invalid_argument("Pattern size must be set explicitly when "
                                "reading from a file-like object");
  }

  auto pattern_bytes = read(size);
  auto pattern_begin = pattern_bytes.begin();
  return kllvm::detail::read(pattern_begin, pattern_bytes.end(), version);
}

void bind_ast(py::module_ &m) {
  auto ast = m.def_submodule("ast", "K LLVM backend KORE AST");

  /* Declarations */
  auto decl_base
      = py::class_<KOREDeclaration, std::shared_ptr<KOREDeclaration>>(
            ast, "Declaration")
            .def("__repr__", print_repr_adapter<KOREDeclaration>())
            .def(
                "add_object_sort_variable",
                &KOREDeclaration::addObjectSortVariable)
            .def_property_readonly(
                "object_sort_variables",
                &KOREDeclaration::getObjectSortVariables)
            .def("add_attribute", &KOREDeclaration::addAttribute)
            .def_property_readonly(
                "attributes", &KOREDeclaration::getAttributes);

  py::class_<
      KORECompositeSortDeclaration,
      std::shared_ptr<KORECompositeSortDeclaration>>(
      ast, "CompositeSortDeclaration", decl_base)
      .def(
          py::init(&KORECompositeSortDeclaration::Create), py::arg("name"),
          py::arg("is_hooked") = false)
      .def_property_readonly(
          "is_hooked", &KORECompositeSortDeclaration::isHooked)
      .def_property_readonly("name", &KORECompositeSortDeclaration::getName);

  auto symbol_alias_decl_base
      = py::class_<
            KORESymbolAliasDeclaration,
            std::shared_ptr<KORESymbolAliasDeclaration>>(
            ast, "SymbolAliasDeclaration", decl_base)
            .def_property_readonly(
                "symbol", &KORESymbolAliasDeclaration::getSymbol);

  py::class_<KORESymbolDeclaration, std::shared_ptr<KORESymbolDeclaration>>(
      ast, "SymbolDeclaration", symbol_alias_decl_base)
      .def(
          py::init(&KORESymbolDeclaration::Create), py::arg("name"),
          py::arg("is_hooked") = false)
      .def_property_readonly("is_hooked", &KORESymbolDeclaration::isHooked);

  py::class_<KOREAliasDeclaration, std::shared_ptr<KOREAliasDeclaration>>(
      ast, "AliasDeclaration", symbol_alias_decl_base)
      .def(py::init(&KOREAliasDeclaration::Create))
      .def("add_variables", &KOREAliasDeclaration::addVariables)
      .def_property_readonly(
          "variables", &KOREAliasDeclaration::getBoundVariables)
      .def("add_pattern", &KOREAliasDeclaration::addPattern)
      .def_property_readonly("pattern", &KOREAliasDeclaration::getPattern);

  py::class_<KOREAxiomDeclaration, std::shared_ptr<KOREAxiomDeclaration>>(
      ast, "AxiomDeclaration", decl_base)
      .def(py::init(&KOREAxiomDeclaration::Create), py::arg("is_claim") = false)
      .def_property_readonly("is_claim", &KOREAxiomDeclaration::isClaim)
      .def("add_pattern", &KOREAxiomDeclaration::addPattern)
      .def_property_readonly("pattern", &KOREAxiomDeclaration::getPattern);

  py::class_<
      KOREModuleImportDeclaration,
      std::shared_ptr<KOREModuleImportDeclaration>>(
      ast, "ModuleImportDeclaration", decl_base)
      .def(py::init(&KOREModuleImportDeclaration::Create))
      .def_property_readonly(
          "module_name", &KOREModuleImportDeclaration::getModuleName);

  py::class_<KOREModule, std::shared_ptr<KOREModule>>(ast, "Module")
      .def(py::init(&KOREModule::Create))
      .def("__repr__", print_repr_adapter<KOREModule>())
      .def_property_readonly("name", &KOREModule::getName)
      .def("add_declaration", &KOREModule::addDeclaration)
      .def_property_readonly("declarations", &KOREModule::getDeclarations)
      .def("add_attribute", &KOREModule::addAttribute)
      .def_property_readonly("attributes", &KOREModule::getAttributes);

  py::class_<KOREDefinition, std::shared_ptr<KOREDefinition>>(ast, "Definition")
      .def(py::init(&KOREDefinition::Create))
      .def("__repr__", print_repr_adapter<KOREDefinition>())
      .def("add_module", &KOREDefinition::addModule)
      .def_property_readonly("modules", &KOREDefinition::getModules)
      .def("add_attribute", &KOREDefinition::addAttribute)
      .def_property_readonly("attributes", &KOREDefinition::getAttributes);

  /* Data Types */

  py::enum_<SortCategory>(ast, "SortCategory")
      .value("Uncomputed", SortCategory::Uncomputed)
      .value("Map", SortCategory::Map)
      .value("RangeMap", SortCategory::RangeMap)
      .value("List", SortCategory::List)
      .value("Set", SortCategory::Set)
      .value("Int", SortCategory::Int)
      .value("Float", SortCategory::Float)
      .value("StringBuffer", SortCategory::StringBuffer)
      .value("Bool", SortCategory::Bool)
      .value("Symbol", SortCategory::Symbol)
      .value("Variable", SortCategory::Variable)
      .value("MInt", SortCategory::MInt);

  py::class_<ValueType>(ast, "ValueType")
      .def(py::init([](SortCategory cat) {
        return ValueType{cat, 0};
      }))
      .def(py::init([](SortCategory cat, uint64_t bits) {
        return ValueType{cat, bits};
      }));

  /* Sorts */

  // The "redundant" expressions here are used by Pybind's metaprogramming to
  // generate equality functions over the bound classes.
  // NOLINTBEGIN(misc-redundant-expression)

  auto sort_base
      = py::class_<KORESort, std::shared_ptr<KORESort>>(ast, "Sort")
            .def_property_readonly("is_concrete", &KORESort::isConcrete)
            .def("substitute", &KORESort::substitute)
            .def("__repr__", print_repr_adapter<KORESort>())
            .def(
                "__hash__",
                [](KORESort const &sort) { return HashSort{}(sort); })
            .def(py::self == py::self)
            .def(py::self != py::self);

  py::class_<KORESortVariable, std::shared_ptr<KORESortVariable>>(
      ast, "SortVariable", sort_base)
      .def(py::init(&KORESortVariable::Create))
      .def_property_readonly("name", &KORESortVariable::getName);

  py::class_<KORECompositeSort, std::shared_ptr<KORECompositeSort>>(
      ast, "CompositeSort", sort_base)
      .def(
          py::init(&KORECompositeSort::Create), py::arg("name"),
          py::arg("cat") = ValueType{SortCategory::Uncomputed, 0})
      .def_property_readonly("name", &KORECompositeSort::getName)
      .def("add_argument", &KORECompositeSort::addArgument)
      .def_property_readonly("arguments", &KORECompositeSort::getArguments);

  /* Symbols */

  py::class_<KORESymbol>(ast, "Symbol")
      .def(py::init(&KORESymbol::Create))
      .def("__repr__", print_repr_adapter<KORESymbol>())
      .def("add_argument", &KORESymbol::addArgument)
      .def_property_readonly("arguments", &KORESymbol::getArguments)
      .def("add_formal_argument", &KORESymbol::addFormalArgument)
      .def_property_readonly(
          "formal_arguments", &KORESymbol::getFormalArguments)
      .def("add_sort", &KORESymbol::addSort)
      .def_property_readonly(
          "sort", py::overload_cast<>(&KORESymbol::getSort, py::const_))
      .def_property_readonly("name", &KORESymbol::getName)
      .def_property_readonly("is_concrete", &KORESymbol::isConcrete)
      .def_property_readonly("is_builtin", &KORESymbol::isBuiltin)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<KOREVariable>(ast, "Variable")
      .def(py::init(&KOREVariable::Create))
      .def("__repr__", print_repr_adapter<KOREVariable>())
      .def_property_readonly("name", &KOREVariable::getName);

  // NOLINTEND(misc-redundant-expression)

  /* Patterns */

  auto pattern_base
      = py::class_<KOREPattern, std::shared_ptr<KOREPattern>>(ast, "Pattern")
            .def(py::init(&KOREPattern::load))
            .def("__repr__", print_repr_adapter<KOREPattern>())
            .def_property_readonly("sort", &KOREPattern::getSort)
            .def("substitute", &KOREPattern::substitute)
            .def(
                "serialize",
                [](KOREPattern const &pattern, bool emit_size) {
                  auto out = serializer{};
                  pattern.serialize_to(out);

                  if (emit_size) {
                    out.correct_emitted_size();
                  }

                  return py::bytes(out.byte_string());
                },
                py::kw_only(), py::arg("emit_size") = false)
            .def_static(
                "deserialize",
                [](py::bytes const &bytes, bool strip_raw_term) {
                  auto str = std::string(bytes);
                  return deserialize_pattern(
                      str.begin(), str.end(), strip_raw_term);
                },
                py::arg("bytes"), py::kw_only(),
                py::arg("strip_raw_term") = true)
            .def_static("read_from", &read_pattern_from_file);

  py::class_<KORECompositePattern, std::shared_ptr<KORECompositePattern>>(
      ast, "CompositePattern", pattern_base)
      .def(py::init(py::overload_cast<std::string const &>(
          &KORECompositePattern::Create)))
      .def(py::init(
          py::overload_cast<KORESymbol *>(&KORECompositePattern::Create)))
      .def_property_readonly(
          "constructor", &KORECompositePattern::getConstructor)
      .def("desugar_associative", &KORECompositePattern::desugarAssociative)
      .def("add_argument", &KORECompositePattern::addArgument)
      .def_property_readonly("arguments", &KORECompositePattern::getArguments);

  py::class_<KOREVariablePattern, std::shared_ptr<KOREVariablePattern>>(
      ast, "VariablePattern", pattern_base)
      .def(py::init(&KOREVariablePattern::Create))
      .def_property_readonly("name", &KOREVariablePattern::getName);

  py::class_<KOREStringPattern, std::shared_ptr<KOREStringPattern>>(
      ast, "StringPattern", pattern_base)
      .def(py::init(&KOREStringPattern::Create))
      .def_property_readonly("contents", &KOREStringPattern::getContents);
}

void bind_parser(py::module_ &mod) {
  auto parser = mod.def_submodule("parser", "KORE Parser");

  py::class_<KOREParser, std::unique_ptr<KOREParser>>(parser, "Parser")
      .def(py::init<std::string>())
      .def_static("from_string", &KOREParser::from_string)
      .def(
          "pattern",
          [](KOREParser &parser) { return std::shared_ptr(parser.pattern()); })
      .def("sort", [](KOREParser &parser) { return parser.sort(); })
      .def("definition", [](KOREParser &parser) {
        return std::shared_ptr(parser.definition());
      });
}

void bind_proof_trace(py::module_ &m) {
  auto proof_trace = m.def_submodule("prooftrace", "K LLVM backend KORE AST");

  auto llvm_step_event
      = py::class_<LLVMStepEvent, std::shared_ptr<LLVMStepEvent>>(
            proof_trace, "LLVMStepEvent")
            .def("__repr__", print_repr_adapter<LLVMStepEvent>());

  auto llvm_rewrite_event
      = py::class_<LLVMRewriteEvent, std::shared_ptr<LLVMRewriteEvent>>(
            proof_trace, "LLVMREwriteEvent", llvm_step_event)
            .def_property_readonly(
                "rule_ordinal", &LLVMRewriteEvent::getRuleOrdinal)
            .def_property_readonly(
                "substitution", &LLVMRewriteEvent::getSubstitution);

  [[maybe_unused]] auto llvm_rule_event
      = py::class_<LLVMRuleEvent, std::shared_ptr<LLVMRuleEvent>>(
          proof_trace, "LLVMRuleEvent", llvm_rewrite_event);

  [[maybe_unused]] auto llvm_side_condition_event = py::class_<
      LLVMSideConditionEvent, std::shared_ptr<LLVMSideConditionEvent>>(
      proof_trace, "LLVMSideConditionEvent", llvm_rewrite_event);

  py::class_<LLVMFunctionEvent, std::shared_ptr<LLVMFunctionEvent>>(
      proof_trace, "LLVMFunctionEvent", llvm_step_event)
      .def_property_readonly("name", &LLVMFunctionEvent::getName)
      .def_property_readonly(
          "relative_position", &LLVMFunctionEvent::getRelativePosition)
      .def_property_readonly("args", &LLVMFunctionEvent::getArguments);

  py::class_<LLVMHookEvent, std::shared_ptr<LLVMHookEvent>>(
      proof_trace, "LLVMHookEvent", llvm_step_event)
      .def_property_readonly("name", &LLVMHookEvent::getName)
      .def_property_readonly(
          "relative_position", &LLVMHookEvent::getRelativePosition)
      .def_property_readonly("args", &LLVMHookEvent::getArguments)
      .def_property_readonly("result", &LLVMHookEvent::getKOREPattern);

  py::class_<LLVMEvent, std::shared_ptr<LLVMEvent>>(proof_trace, "Argument")
      .def("__repr__", print_repr_adapter<LLVMEvent>(true))
      .def_property_readonly("step_event", &LLVMEvent::getStepEvent)
      .def_property_readonly("kore_pattern", &LLVMEvent::getKOREPattern)
      .def("is_step_event", &LLVMEvent::isStep)
      .def("is_kore_pattern", &LLVMEvent::isPattern);

  py::class_<LLVMRewriteTrace, std::shared_ptr<LLVMRewriteTrace>>(
      proof_trace, "LLVMRewriteTrace")
      .def("__repr__", print_repr_adapter<LLVMRewriteTrace>())
      .def_property_readonly("version", &LLVMRewriteTrace::getVersion)
      .def_property_readonly("pre_trace", &LLVMRewriteTrace::getPreTrace)
      .def_property_readonly(
          "initial_config", &LLVMRewriteTrace::getInitialConfig)
      .def_property_readonly("trace", &LLVMRewriteTrace::getTrace)
      .def_static(
          "parse",
          [](py::bytes const &bytes) {
            ProofTraceParser Parser(false);
            auto str = std::string(bytes);
            return Parser.parse_proof_trace(str);
          },
          py::arg("bytes"));
}

PYBIND11_MODULE(_kllvm, m) {
  bind_ast(m);
  bind_parser(m);
  bind_proof_trace(m);
}
