/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PeepholeV2.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "RedundantCheckCastRemover.h"
#include "Walkers.h"

////////////////////////////////////////////////////////////////////////////////
// PeepholeOptimizerV2 implementation
//
// Overview of the algorithm: Say we have the following code patterns to match
// and replace, and code sequence, where ; denotes basic block boundaries.
//
//           |  Match  | Replace |
// Pattern 1 |  a b c  |    x    |
// Pattern 2 |  a b d  |   y z   |
//
// Before:  ; a a b c a b d a f b d a b ; c a b d ;
//              ~~~~~ ~~~~~                 ~~~~~
// After:   ; a   x    y z  a f b d a b ; c  y z ;
//
// Assumptions:
//  (1) 'match' patterns do not span over multiple basic blocks as of now.
//      We may relax this restriction later.
//  (2) 'match' patterns cannot be interleaved by other instructions. In the
//      above example, "a f b d" won't be matched to "a b d" because of 'f'.
//      The current peephole implementation allows such interleaving as the
//      algorithm keeps track of data flow instead of pattern matching.
//
// This is essentially a string searching problem. We can ideally utilize
// std::search. But a full-fledged searching even with an optimal algorithm
// (e.g., Boyer-Moore) would take some time. ProGuard's approach is very
// interesting. Instead of a thorough searching, they applied a really simple
// heuristic when matching fails. For instance:
//
// Code:     a a b c     a a b c
//           | |           |
//           o x    ===>   o (retry)  ===> "a b c" will be matched
//           | |           |
// Pattern:  a b c         a b c   Only if matching fails on the second opcode
//                                 of the pattern, it retries to match the
//                                 current opcode and the pattern.
//
// Code:     a b a b c    a b a b c
//           | | |              |
//           o o x    ===>      x ..  ===> "a b c" won't be matched
//           | | |              |
// Pattern:  a b c              a  No retry. No rescan. Search resumes from the
//                                 the next opcode.
//
// So, on a matching failure, PG only retries when the failure occurs on the
// second opcode of the pattern. Otherwise, it simply moves forward. I would say
// this heuristic as a "sweeping" or "try-and-forget" algorithm because it only
// scans the code one time with very minimal retry. We first implement this PG's
// approach. (I don't know whether this is really intended or a bug.)
//
namespace {

// The peephole first detects code patterns like "const-string v0, "foo"".
// We need identifiers to describe the arguments of each instruction such as
// registers, method, literals, etc. For instance, we need an identifier for
// an arbitrary literal argument. We may need an identifier only for an empty
// string.
//
// Once a pattern is detected, the original instructions are replaced by new
// instructions. Sometimes we need to patch the arguments of the new
// instructions. For instance, we want to write the length of string A.
// We also need a special identifier for this action.
//
enum class Register : uint16_t {
  // It reserves only even numbers for wide pairs.
  A = 1,
  B = 3,
  C = 5,
  D = 7,

  pair_A = 2,
  pair_B = 4,
  pair_C = 6,
  pair_D = 8,
};

// The size of an array enabling us to index by Register
static constexpr size_t kRegisterArraySize = 9;

Register get_pair_register(Register reg) {
  assert(reg == Register::A || reg == Register::B || reg == Register::C ||
         reg == Register::D);
  return Register(uint16_t(reg) + 1);
}

enum class Literal {
  // For an arbitrary literal argument
  A,
  // Directive: Compare strings A and B and write the result as a 4-bit integer.
  Compare_Strings_A_B,
  // Directive: Write the length of string A as a 16-bit integer.
  Length_String_A,
};

enum class String {
  // For arbitrary string arguments
  A,
  B,
  // For only an empty string argument
  empty,

  // Special string argument directives for replacements
  boolean_A_to_string, // e.g., convert literal A as a boolean to a string.
  char_A_to_string,
  int_A_to_string,
  long_int_A_to_string,
  float_A_to_string,
  double_A_to_string,
  concat_A_B_strings,
  concat_string_A_boolean_A,
  concat_string_A_char_A,
  concat_string_A_int_A,
  concat_string_A_long_int_A,
  Type_A_get_simple_name,
};

enum class Type {
  A,
  B,
};

// Just a minimal refactor for long string constants.
static const char* LjavaString = "Ljava/lang/String;";
static const char* LjavaStringBuilder = "Ljava/lang/StringBuilder;";
static const char* LjavaObject = "Ljava/lang/Object;";

struct DexPattern {
  const std::unordered_set<uint16_t> opcodes;
  const std::vector<Register> srcs;
  const std::vector<Register> dests;

  const enum class Kind {
    none,
    method,
    string,
    literal,
    type,
    copy, // Replace with the same exact instruction we matched. No change.
  } kind;

  const union {
    std::nullptr_t const dummy;
    DexMethod* const method;
    String const string;
    Literal const literal;
    Type const type;
    unsigned int const copy_index;
  };

  DexPattern(const std::unordered_set<uint16_t>& opcodes,
             const std::vector<Register>& srcs,
             const std::vector<Register>& dests)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::none),
        dummy(nullptr) {}

  DexPattern(const std::unordered_set<uint16_t>& opcodes,
             const std::vector<Register>& srcs,
             const std::vector<Register>& dests,
             DexMethod* const method)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::method),
        method(method) {}

  DexPattern(const std::unordered_set<uint16_t>& opcodes,
             const std::vector<Register>& srcs,
             const std::vector<Register>& dests,
             String const string)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::string),
        string(string) {}

  DexPattern(const std::unordered_set<uint16_t>& opcodes,
             const std::vector<Register>& srcs,
             const std::vector<Register>& dests,
             Literal const literal)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::literal),
        literal(literal) {}

  DexPattern(const std::unordered_set<uint16_t>& opcodes,
             const std::vector<Register>& srcs,
             const std::vector<Register>& dests,
             Type const type)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::type),
        type(type) {}

  static const DexPattern copy_matched_instruction(int index) {
    return DexPattern(index);
  }

 private:
  explicit DexPattern(unsigned int index)
      : kind(Kind::copy), copy_index(index) {}
};

struct Matcher;

// Returns the smallest bit width of any source or destination vreg for the
// given opcode
// Returns 16 (no limit) if there is no source or destination (e.g. nop)
static int8_t min_vreg_bit_width_for_opcode(uint16_t opcode) {
  IRInstruction insn((DexOpcode)opcode);
  int result = 16;
  if (insn.dests_size() > 0) {
    result = std::min(result, dest_bit_width(insn.opcode()));
  }
  for (unsigned i = 0; i < insn.srcs_size(); i++) {
    result = std::min(result, src_bit_width(insn.opcode(), i));
  }
  return static_cast<int8_t>(result);
}

struct Pattern {
  const std::string name;
  const std::vector<DexPattern> match;
  const std::vector<DexPattern> replace;
  const std::function<bool(const Matcher&)> predicate;
  std::array<int8_t, kRegisterArraySize> register_width_limits;

 private:
  void determine_register_width_limits() {
    // We need to ensure we don't match registers that exceed the bit width of
    // the replacement instruction
    // Most instructions have the same bit width for source and dest, so we just
    // calculate a single
    // minimum bit width for each instruction
    register_width_limits.fill(16); // default
    for (const DexPattern& pat : replace) {
      for (uint16_t opcode : pat.opcodes) { // just expect 1
        int8_t width = min_vreg_bit_width_for_opcode(opcode);
        for (Register reg : pat.srcs) {
          int idx = static_cast<int>(reg);
          register_width_limits[idx] =
              std::min(register_width_limits[idx], width);
        }
        for (Register reg : pat.dests) {
          int idx = static_cast<int>(reg);
          register_width_limits[idx] =
              std::min(register_width_limits[idx], width);
        }
      }
    }
  }

 public:
  Pattern(std::string name,
          std::vector<DexPattern> match,
          std::vector<DexPattern> replace,
          std::function<bool(const Matcher&)> predicate = {})
      : name(std::move(name)),
        match(std::move(match)),
        replace(std::move(replace)),
        predicate(std::move(predicate)) {
    determine_register_width_limits();
  }

  // Returns whether the given vreg value is suitable for the register pattern
  bool register_can_match_vreg_value(Register pattern, uint16_t value) const {
    int limit = register_width_limits[static_cast<int>(pattern)];
    return value < (1U << limit);
  }
};

// Matcher holds the matching state for the given pattern.
struct Matcher {
  const Pattern& pattern;
  size_t match_index;
  std::vector<IRInstruction*> matched_instructions;

  // Another reason why we need C++14...
  struct EnumClassHash {
    template <typename T>
    size_t operator()(T t) const {
      return static_cast<size_t>(t);
    }
  };

  std::unordered_map<Register, uint16_t, EnumClassHash> matched_regs;
  std::unordered_map<String, DexString*, EnumClassHash> matched_strings;
  std::unordered_map<Literal, int64_t, EnumClassHash> matched_literals;
  std::unordered_map<Type, DexType*, EnumClassHash> matched_types;

  explicit Matcher(const Pattern& pattern) : pattern(pattern), match_index(0) {}

  void reset() {
    match_index = 0;
    matched_instructions.clear();
    matched_regs.clear();
    matched_strings.clear();
    matched_literals.clear();
    matched_types.clear();
  }

  // It updates the matching state for the given instruction. Returns true if
  // insn matches to the last 'match' pattern.
  bool try_match(IRInstruction* insn) {
    auto match_reg = [&](Register pattern_reg, uint16_t insn_reg) {
      // This register has been observed already. Check whether they are same.
      if (matched_regs.find(pattern_reg) != end(matched_regs)) {
        return matched_regs.at(pattern_reg) == insn_reg;
      }

      // Refuse to match if the register exceeds the instruction's width limit
      if (!pattern.register_can_match_vreg_value(pattern_reg, insn_reg)) {
        return false;
      }

      // Newly observed. Remember it.
      matched_regs.emplace(pattern_reg, insn_reg);
      return true;
    };

    auto match_literal = [&](Literal pattern, int64_t insn_literal_val) {
      if (matched_literals.find(pattern) != end(matched_literals)) {
        return matched_literals.at(pattern) == insn_literal_val;
      }
      matched_literals.emplace(pattern, insn_literal_val);
      return true;
    };

    auto match_string = [&](String pattern, DexString* insn_str) {
      if (pattern == String::empty) {
        return (insn_str->is_simple() && insn_str->size() == 0);
      }
      if (matched_strings.find(pattern) != end(matched_strings)) {
        return matched_strings.at(pattern) == insn_str;
      }
      matched_strings.emplace(pattern, insn_str);
      return true;
    };

    auto match_type = [&](Type pattern, DexType* insn_type) {
      if (matched_types.find(pattern) != end(matched_types)) {
        return matched_types.at(pattern) == insn_type;
      }
      matched_types.emplace(pattern, insn_type);
      return true;
    };

    // Does 'insn' match to the given DexPattern?
    auto match_instruction = [&](const DexPattern& pattern) {
      if (pattern.opcodes.find(insn->opcode()) == end(pattern.opcodes) ||
          pattern.srcs.size() != insn->srcs_size() ||
          pattern.dests.size() != insn->dests_size()) {
        return false;
      }

      if (pattern.dests.size() != 0) {
        assert(pattern.dests.size() == 1);
        if (!match_reg(pattern.dests[0], insn->dest())) {
          return false;
        }
      }

      for (size_t i = 0; i < pattern.srcs.size(); ++i) {
        if (!match_reg(pattern.srcs[i], insn->src(i))) {
          return false;
        }
      }

      switch (pattern.kind) {
      case DexPattern::Kind::none:
        return true;
      case DexPattern::Kind::string:
        return match_string(
            pattern.string,
            static_cast<const IRStringInstruction*>(insn)->get_string());
      case DexPattern::Kind::literal:
        return match_literal(pattern.literal, insn->literal());
      case DexPattern::Kind::method:
        return pattern.method ==
               static_cast<const IRMethodInstruction*>(insn)->get_method();
      case DexPattern::Kind::type:
        return match_type(
            pattern.type,
            static_cast<const IRTypeInstruction*>(insn)->get_type());
      case DexPattern::Kind::copy:
        always_assert_log(
            false, "Kind::copy can only be used in replacements. Not matches");
      }
      return false;
    };

    assert(match_index < pattern.match.size());
    if (!match_instruction(pattern.match[match_index])) {
      // Okay, this is the PG's heuristic. Retry only if the failure occurs on
      // the second opcode of the pattern.
      bool retry = (match_index == 1);
      TRACE(PEEPHOLE,
            8,
            "Not Matched: %s[%lu] != %s\n",
            pattern.name.c_str(),
            match_index,
            SHOW(insn));
      reset();
      if (retry) {
        assert(match_index == 0);
        if (!match_instruction(pattern.match[match_index])) {
          return false;
        }
      } else {
        return false;
      }
    }

    TRACE(PEEPHOLE,
          8,
          "Matched [%lu/%lu]: %s\n",
          match_index + 1,
          pattern.match.size(),
          SHOW(insn));
    matched_instructions.push_back(insn);
    ++match_index;

    bool done = match_index == pattern.match.size();

    // if we've matched everything, the predicate may still veto
    if (done && pattern.predicate && !pattern.predicate(*this)) {
      reset();
      return false;
    }
    return done;
  }

  // Generate skeleton instruction for the replacement.
  IRInstruction* generate_dex_instruction(const DexPattern& replace) {
    if (replace.opcodes.size() != 1) {
      always_assert_log(false, "Replacement must have unique opcode");
      return nullptr;
    }

    const auto opcode = *begin(replace.opcodes);
    switch (opcode) {
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_VIRTUAL:
      assert(replace.kind == DexPattern::Kind::method);
      return (new IRMethodInstruction((DexOpcode)opcode, replace.method))
          ->set_arg_word_count(replace.srcs.size());

    case OPCODE_MOVE_16:
      assert(replace.kind == DexPattern::Kind::none);
      return new IRInstruction((DexOpcode)opcode);

    case OPCODE_MOVE_RESULT:
    case OPCODE_MOVE_RESULT_OBJECT:
      assert(replace.kind == DexPattern::Kind::none);
      return new IRInstruction((DexOpcode)opcode);

    case OPCODE_NEG_INT:
      assert(replace.kind == DexPattern::Kind::none);
      return new IRInstruction((DexOpcode)opcode);

    case OPCODE_CONST_STRING:
      assert(replace.kind == DexPattern::Kind::string);
      return new IRStringInstruction(OPCODE_CONST_STRING, nullptr);

    case OPCODE_CONST_4:
    case OPCODE_CONST_16:
    case OPCODE_CONST:
      assert(replace.kind == DexPattern::Kind::literal);
      return new IRInstruction((DexOpcode)opcode);
    }

    always_assert_log(false, "Unhandled opcode: 0x%x", opcode);
    return nullptr;
  }

  DexString* get_simple_name(const DexType* type) {
    std::string full(type->get_name()->c_str());
    auto lpos = full.rfind('/');
    auto simple = full.substr(lpos + 1, full.size() - lpos - 2);
    return DexString::make_string(simple.c_str());
  }

  // After a successful match, get the replacement instructions. We substitute
  // the placeholders appropriately including special command placeholders.
  std::vector<IRInstruction*> get_replacements() {
    always_assert(pattern.match.size() == match_index);

    std::vector<IRInstruction*> replacements;
    for (const auto& replace_info : pattern.replace) {
      // First, generate the instruction object.
      if (replace_info.kind == DexPattern::Kind::copy) {
        always_assert(matched_instructions.size() > replace_info.copy_index);
        replacements.push_back(
            matched_instructions[replace_info.copy_index]->clone());
        continue;
      }
      auto replace = generate_dex_instruction(replace_info);
      replacements.push_back(replace);

      // Fill the arguments appropriately.
      if (replace_info.dests.size() > 0) {
        assert(replace_info.dests.size() == 1);
        const Register dest = replace_info.dests[0];
        always_assert(matched_regs.find(dest) != end(matched_regs));
        replace->set_dest(matched_regs.at(dest));
      }

      for (size_t i = 0; i < replace_info.srcs.size(); ++i) {
        const Register reg = replace_info.srcs[i];
        always_assert(matched_regs.find(reg) != end(matched_regs));
        replace->set_src(i, matched_regs.at(reg));
      }

      if (replace_info.kind == DexPattern::Kind::string) {
        switch (replace_info.string) {
        case String::A: {
          auto a = matched_strings.at(String::A);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(a);
          break;
        }
        case String::boolean_A_to_string: {
          bool a = matched_literals.at(Literal::A);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(a == true ? "true" : "false"));
          break;
        }
        case String::char_A_to_string: {
          int a = matched_literals.at(Literal::A);
          auto achar = encode_utf8_char_to_mutf8_string(a);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(achar.c_str(), 1));
          break;
        }
        case String::int_A_to_string: {
          int a = matched_literals.at(Literal::A);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::to_string(a)));
          break;
        }
        case String::long_int_A_to_string: {
          int64_t a = matched_literals.at(Literal::A);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::to_string(a)));
          break;
        }
        case String::float_A_to_string: {
          union {
            int32_t i;
            float f;
          } a;
          a.i = static_cast<int32_t>(matched_literals.at(Literal::A));
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::to_string(a.f)));
          break;
        }
        case String::double_A_to_string: {
          union {
            int64_t i;
            double d;
          } a;
          a.i = matched_literals.at(Literal::A);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::to_string(a.d)));
          break;
        }
        case String::concat_A_B_strings: {
          auto a = matched_strings.at(String::A)->c_str();
          auto b = matched_strings.at(String::B)->c_str();
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::string(a) + std::string(b)));
          break;
        }
        case String::concat_string_A_int_A: {
          auto a = matched_strings.at(String::A)->c_str();
          int b = matched_literals.at(Literal::A);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::string(a) + std::to_string(b)));
          break;
        }
        case String::concat_string_A_boolean_A: {
          auto a = matched_strings.at(String::A)->c_str();
          bool b = matched_literals.at(Literal::A);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::string(a) +
                                     (b == true ? "true" : "false")));
          break;
        }
        case String::concat_string_A_long_int_A: {
          auto a = matched_strings.at(String::A)->c_str();
          int64_t b = matched_literals.at(Literal::A);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::string(a) + std::to_string(b)));
          break;
        }
        case String::concat_string_A_char_A: {
          auto a = matched_strings.at(String::A)->c_str();
          int b = matched_literals.at(Literal::A);
          auto bchar = encode_utf8_char_to_mutf8_string(b);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(
              DexString::make_string(std::string(a) + bchar));
          break;
        }
        case String::Type_A_get_simple_name: {
          DexType* a = matched_types.at(Type::A);
          DexString* simple = get_simple_name(a);
          static_cast<IRStringInstruction*>(replace)->rewrite_string(simple);
          break;
        }
        default:
          always_assert_log(
              false, "Unexpected string directive: 0x%x", replace_info.string);
          break;
        }
      } else if (replace_info.kind == DexPattern::Kind::literal) {
        switch (replace_info.literal) {
        case Literal::Compare_Strings_A_B: {
          auto a = matched_strings.at(String::A);
          auto b = matched_strings.at(String::B);
          // Just DexString* pointer comparison! DexString has uniqueness.
          replace->set_literal((a == b) ? 1L : 0L);
          break;
        }
        case Literal::Length_String_A: {
          auto a = matched_strings.at(String::A);
          replace->set_literal(a->length());
          break;
        }
        case Literal::A: {
          auto a = matched_literals.at(Literal::A);
          replace->set_literal(a);
          break;
        }
        default:
          always_assert_log(
              false, "Unexpected literal directive 0x%x", replace_info.literal);
          break;
        }
      } else if (replace_info.kind == DexPattern::Kind::type) {
        auto replace_type = static_cast<IRTypeInstruction*>(replace);
        switch (replace_info.type) {
        case Type::A:
          replace_type->rewrite_type(matched_types.at(Type::A));
          break;
        case Type::B:
          replace_type->rewrite_type(matched_types.at(Type::B));
          break;
        default:
          always_assert_log(
              false, "Unexpected type directive 0x%x", replace_info.type);
          break;
        }
      }
    }
    return replacements;
  }
};

////////////////////////////////////////////////////////////////////////////////
namespace patterns {
// Helpers
//
// invoke-direct {reg_instance}, Ljava/lang/StringBuilder;.<init>:()V
DexPattern invoke_StringBuilder_init(Register instance) {
  return {{OPCODE_INVOKE_DIRECT},
          {instance},
          {},
          DexMethod::make_method(LjavaStringBuilder, "<init>", "V", {})};
}

// invoke-direct {reg_instance, reg_argument},
// Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V
DexPattern invoke_StringBuilder_init_String(Register instance,
                                            Register argument) {
  return {
      {OPCODE_INVOKE_DIRECT},
      {instance, argument},
      {},
      DexMethod::make_method(LjavaStringBuilder, "<init>", "V", {LjavaString})};
};

// invoke-virtual {reg_instance, reg_argument},
// Ljava/lang/StringBuilder;.append:(param_type)Ljava/lang/StringBuilder;
DexPattern invoke_StringBuilder_append(Register instance,
                                       Register argument,
                                       const char* param_type) {
  std::vector<Register> srcs;
  if (strcmp(param_type, "J") == 0 || strcmp(param_type, "D") == 0) {
    srcs = {instance, argument, get_pair_register(argument)};
  } else {
    srcs = {instance, argument};
  }
  return {{OPCODE_INVOKE_VIRTUAL},
          std::move(srcs),
          {},
          DexMethod::make_method(
              LjavaStringBuilder, "append", LjavaStringBuilder, {param_type})};
};

DexPattern invoke_String_valueOf(Register argument, const char* param_type) {
  std::vector<Register> srcs;
  if (strcmp(param_type, "J") == 0 || strcmp(param_type, "D") == 0) {
    srcs = {argument, get_pair_register(argument)};
  } else {
    srcs = {argument};
  }
  return {{OPCODE_INVOKE_STATIC},
          std::move(srcs),
          {},
          DexMethod::make_method(
              LjavaString, "valueOf", LjavaString, {param_type})};
};

DexPattern invoke_String_equals(Register instance, Register argument) {
  return {{OPCODE_INVOKE_VIRTUAL},
          {instance, argument},
          {},
          DexMethod::make_method(LjavaString, "equals", "Z", {LjavaObject})};
};

DexPattern invoke_String_length(Register instance) {
  return {{OPCODE_INVOKE_VIRTUAL},
          {instance},
          {},
          DexMethod::make_method(LjavaString, "length", "I", {})};
};

DexPattern const_string(Register dest, String string) {
  return {{OPCODE_CONST_STRING}, {}, {dest}, string};
};

DexPattern move_result_object(Register dest) {
  return {{OPCODE_MOVE_RESULT_OBJECT}, {}, {dest}};
};

DexPattern move_result(Register dest) {
  return {{OPCODE_MOVE_RESULT}, {}, {dest}};
};

DexPattern const_literal(uint16_t opcode, Register dest, Literal literal) {
  return {{opcode}, {}, {dest}, literal};
};

DexPattern const_wide(Register dest, Literal literal) {
  return {{OPCODE_CONST_WIDE_16, OPCODE_CONST_WIDE_32, OPCODE_CONST_WIDE},
          {},
          {dest},
          literal};
};

DexPattern const_integer(Register dest, Literal literal) {
  return {{OPCODE_CONST_4, OPCODE_CONST_16, OPCODE_CONST}, {}, {dest}, literal};
};

DexPattern const_float(Register dest, Literal literal) {
  return {{OPCODE_CONST_4, OPCODE_CONST}, {}, {dest}, literal};
};

DexPattern const_char(Register dest, Literal literal) {
  // Modified UTF-8, 1-3 bytes. DX uses const/4 for the null character
  // (\u0000), and const/16 and const to load a char.
  return const_integer(dest, literal);
};

static const std::vector<Pattern>& get_string_patterns() {
  static const std::vector<Pattern> kStringPatterns = {
      // It coalesces init(void) and append(string) into init(string).
      // new StringBuilder().append("...") = new StringBuilder("...")
      {"Coalesce_InitVoid_AppendString",
       {invoke_StringBuilder_init(Register::A),
        const_string(Register::B, String::A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::A)},
       {// (3 + 2 + 3 + 1) - (2 + 3) = 4 code unit saving
        const_string(Register::B, String::A),
        invoke_StringBuilder_init_String(Register::A, Register::B)}},

      // It coalesces consecutive two append(string) to a single append call.
      // StringBuilder.append("A").append("B") = StringBuilder.append("AB")
      {"Coalesce_AppendString_AppendString",
       {const_string(Register::B, String::A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_string(Register::D, String::B),
        invoke_StringBuilder_append(Register::C, Register::D, LjavaString)},
       {// 6 code unit saving
        const_string(Register::B, String::concat_A_B_strings),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString)}},

      // It evaluates the length of a literal in compile time.
      // "stringA".length() ==> length_of_stringA
      {"CompileTime_StringLength",
       {const_string(Register::A, String::A),
        invoke_String_length(Register::A),
        move_result(Register::B)},
       {// 2 code unit saving, but potentially String::A can be dead code.
        const_string(Register::A, String::A),
        const_literal(OPCODE_CONST_16, Register::B, Literal::Length_String_A)}},

      // DISABLED: TODO: Found a crash, causing VerifyError
      // It removes an append call with an empty string.
      // StringBuilder.append("") = nothing
      // {"Remove_AppendEmptyString",
      //  {const_string(Register::B, String::empty),
      //   invoke_StringBuilder_append(Register::A, Register::B, LjavaString)},
      //  {}},

      // It coalesces init(void) and append(char) into init(string).
      // StringBuilder().append(C) = new StringBuilder("....")
      {"Coalesce_Init_AppendChar",
       {invoke_StringBuilder_init(Register::A),
        const_char(Register::B, Literal::A),
        invoke_StringBuilder_append(Register::A, Register::B, "C"),
        move_result_object(Register::A)},
       {// (3 + [1, 2, 3] + 3 + 1) - (2 + 3) = [3, 4, 5] code unit saving
        const_string(Register::B, String::char_A_to_string),
        invoke_StringBuilder_init_String(Register::A, Register::B)}},

      // It coalesces append(string) and append(integer) into append(string).
      // StringBuilder.append("...").append(I) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendInt",
       {const_string(Register::B, String::A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_integer(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "I")},
       {// (2 + 3 + 1 + [1, 2, 3] + 3) - (2 + 3) = [5, 6, 7] code unit saving
        const_string(Register::B, String::concat_string_A_int_A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString)}},

      // It coalesces append(string) and append(char) into append(string).
      // StringBuilder.append("...").append(C) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendChar",
       {const_string(Register::B, String::A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_char(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "C")},
       {// (2 + 3 + 1 + [1, 2, 3] + 3) - (2 + 3) = [5, 6, 7] code unit saving
        const_string(Register::B, String::concat_string_A_char_A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString)}},

      // It coalesces append(string) and append(boolean) into append(string).
      // StringBuilder.append("...").append(Z) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendBoolean",
       {const_string(Register::B, String::A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_literal(OPCODE_CONST_4, Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "Z")},
       {// (2 + 3 + 1 + 1 + 3) - (2 + 3) = 5 code unit saving
        const_string(Register::B, String::concat_string_A_boolean_A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString)}},

      // It coalesces append(string) and append(long int) into append(string).
      // StringBuilder.append("...").append(J) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendLongInt",
       {const_string(Register::B, String::A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_wide(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "J")},
       {// (2 + 3 + 1 + [2, 3, 5] + 3) - (2 + 3) = [6, 7, 9] code unit saving
        const_string(Register::B, String::concat_string_A_long_int_A),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString)}},

      // It evaluates the identify of two literal strings in compile time.
      // "stringA".equals("stringB") ==> true or false
      {"CompileTime_StringCompare",
       {const_string(Register::A, String::A),
        const_string(Register::B, String::B),
        invoke_String_equals(Register::A, Register::B),
        move_result(Register::C)},
       {// (2 + 2 + 3 + 1) - 1 = 7 code unit saving
        const_literal(
            OPCODE_CONST_4, Register::C, Literal::Compare_Strings_A_B)}},

      // It replaces valueOf on a boolean value by "true" or "false" directly.
      // String.valueof(true/false) ==> "true" or "false"
      {"Replace_ValueOfBoolean",
       {const_literal(OPCODE_CONST_4, Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "Z"),
        move_result_object(Register::B)},
       {// (1 + 3 + 1) - 2 = 3 16-bit code units saving
        const_string(Register::B, String::boolean_A_to_string)}},

      // It replaces valueOf on a literal character by the character itself.
      // String.valueOf(char) ==> "char"
      {"Replace_ValueOfChar",
       {const_char(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "C"),
        move_result_object(Register::B)},
       {// ([1, 2, 3] + 3 + 1) - 2 = [3, 4, 5] units saving
        const_string(Register::B, String::char_A_to_string)}},

      // It replaces valueOf on an integer literal by the integer itself.
      // String.valueof(int) ==> "int"
      {"Replace_ValueOfInt",
       {const_integer(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "I"),
        move_result_object(Register::B)},
       {// ([1, 2, 3] + 3 + 1) - 2 = [3, 4, 5] units saving
        const_string(Register::B, String::int_A_to_string)}},

      // It replaces valueOf on a long integer literal by the number itself.
      // String.valueof(long int) ==> "long int"
      {"Replace_ValueOfLongInt",
       {const_wide(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "J"),
        move_result_object(Register::B)},
       {// ([2, 3, 5] + 3 + 1) - 2 = [4, 5, 7] units saving
        const_string(Register::B, String::long_int_A_to_string)}},

      // It replaces valueOf on a float literal by the float itself.
      // String.valueof(float) ==> "float"
      {"Replace_ValueOfFloat",
       {const_float(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "F"),
        move_result_object(Register::B)},
       {// ([1, 3] + 3 + 1) - 2 = [3, 5] units saving
        const_string(Register::B, String::float_A_to_string)}},

      // It replaces valueOf on a double literal by the double itself.
      // String.valueof(double) ==> "double"
      {"Replace_ValueOfDouble",
       {const_wide(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "D"),
        move_result_object(Register::B)},
       {// ([2, 3, 5] + 3 + 1) - 2 = [4, 5, 7] units saving
        const_string(Register::B, String::double_A_to_string)}},
  };
  return kStringPatterns;
}

DexPattern move_ops(Register dest, Register src) {
  return {{OPCODE_MOVE, OPCODE_MOVE_OBJECT}, {src}, {dest}};
};

const std::vector<Pattern>& get_nop_patterns() {
  static const std::vector<Pattern> kNopPatterns = {
      // Remove redundant move and move_object instructions,
      // e.g. move v0, v0
      {"Remove_Redundant_Move", {move_ops(Register::A, Register::A)}, {}},
  };
  return kNopPatterns;
}

template <int64_t VALUE>
static bool first_instruction_literal_is(const Matcher& m) {
  if (m.matched_instructions.empty()) {
    return false;
  }
  return m.matched_instructions.front()->literal() == VALUE;
}

DexPattern mul_or_div_lit(Register src, Register dst) {
  return {{OPCODE_MUL_INT_LIT8,
           OPCODE_MUL_INT_LIT16,
           OPCODE_DIV_INT_LIT8,
           OPCODE_DIV_INT_LIT16},
          {src},
          {dst}};
};

DexPattern add_lit(Register src, Register dst) {
  return {{OPCODE_ADD_INT_LIT8, OPCODE_ADD_INT_LIT16}, {src}, {dst}};
};

const std::vector<Pattern>& get_arith_patterns() {
  // Note: these arith patterns emit full 16-bit reg indices
  // Another pass will tighten these when possible
  static const std::vector<Pattern> kArithPatterns = {
      // Replace *1 or /1 with move
      {"Arith_MulDivLit_Pos1",
       {mul_or_div_lit(Register::A, Register::B)},
       {// x = y * 1 -> x = y
        {{OPCODE_MOVE_16}, {Register::A}, {Register::B}}},
       first_instruction_literal_is<1>},

      // Replace multiplies or divides by -1 with negation
      {"Arith_MulDivLit_Neg1",
       {mul_or_div_lit(Register::A, Register::B)},
       {// Eliminates the literal-carrying halfword
        {{OPCODE_NEG_INT}, {Register::A}, {Register::B}}},
       first_instruction_literal_is<-1>},

      // Replace +0 with moves
      {"Arith_AddLit_0",
       {add_lit(Register::A, Register::B)},
       {// Eliminates the literal-carrying halfword
        {{OPCODE_MOVE_16}, {Register::A}, {Register::B}}},
       first_instruction_literal_is<0>},
  };
  return kArithPatterns;
}

const std::unordered_set<uint16_t> kAnyInvoke = {OPCODE_INVOKE_VIRTUAL,
                                                 OPCODE_INVOKE_SUPER,
                                                 OPCODE_INVOKE_DIRECT,
                                                 OPCODE_INVOKE_STATIC,
                                                 OPCODE_INVOKE_INTERFACE,
                                                 OPCODE_INVOKE_VIRTUAL_RANGE,
                                                 OPCODE_INVOKE_SUPER_RANGE,
                                                 OPCODE_INVOKE_DIRECT_RANGE,
                                                 OPCODE_INVOKE_STATIC_RANGE,
                                                 OPCODE_INVOKE_INTERFACE_RANGE};

const DexPattern invoke_class_get_simple_name() {
  return {kAnyInvoke,
          {Register::A},
          {},
          DexMethod::make_method(
              "Ljava/lang/Class;", "getSimpleName", "Ljava/lang/String;", {})};
}

DexPattern const_class(Register dest_reg, Type type) {
  return {{OPCODE_CONST_CLASS}, {}, {dest_reg}, type};
};

const std::vector<Pattern>& get_func_patterns() {
  static const std::vector<Pattern> kFuncPatterns = {
      {"Remove_LangClass_GetSimpleName",
       {const_class(Register::A, Type::A),
        invoke_class_get_simple_name(),
        move_result_object(Register::B)},
       {DexPattern::copy_matched_instruction(0), // const_class (maybe dead)
        const_string(Register::B, String::Type_A_get_simple_name)}},
  };
  return kFuncPatterns;
}

const std::vector<std::vector<Pattern>>& get_all_patterns() {
  static const std::vector<std::vector<Pattern>>& kAllPatterns = {
      get_nop_patterns(),
      get_string_patterns(),
      get_arith_patterns(),
      get_func_patterns()};
  return kAllPatterns;
}
}

template <typename T>
bool contains(const std::vector<T>& vec, const T& value) {
  return std::find(vec.begin(), vec.end(), value) != vec.end();
}

class PeepholeOptimizerV2 {
 private:
  const std::vector<DexClass*>& m_scope;
  std::vector<Matcher> m_matchers;
  std::vector<int> m_matchers_stat;
  int m_stats_removed = 0;
  int m_stats_inserted = 0;

 public:
  explicit PeepholeOptimizerV2(const std::vector<DexClass*>& scope,
                               const std::vector<std::string>& disabled_peepholes)
      : m_scope(scope) {
    for (const auto& pattern_list : patterns::get_all_patterns()) {
      for (const Pattern& pattern : pattern_list) {
        if (!contains(disabled_peepholes, pattern.name)) {
          m_matchers.emplace_back(pattern);
          m_matchers_stat.push_back(0);
        } else {
          TRACE(PEEPHOLE,
                2,
                "not running disabled peephole opt %s\n",
                pattern.name.c_str());
        }
      }
    }
  }

  void peephole(DexMethod* method) {
    auto transform = method->get_code()->get_entries();
    transform->build_cfg();

    std::vector<IRInstruction*> deletes;
    std::vector<std::pair<IRInstruction*, std::vector<IRInstruction*>>>
        inserts;
    const auto& blocks = transform->cfg().blocks();
    for (const auto& block : blocks) {
      // Currently, all patterns do not span over multiple basic blocks. So
      // reset all matching states on visiting every basic block.
      for (auto& m : m_matchers) {
        m.reset();
      }

      for (auto& mei : *block) {
        if (mei.type != MFLOW_OPCODE) {
          continue;
        }

        for (size_t i = 0; i < m_matchers.size(); ++i) {
          auto& matcher = m_matchers[i];
          if (!matcher.try_match(mei.insn)) {
            continue;
          }

          assert(i < m_matchers.size());
          ++m_matchers_stat[i];
          TRACE(PEEPHOLE, 8, "PATTERN MATCHED!\n");
          deletes.insert(end(deletes),
                         begin(matcher.matched_instructions),
                         end(matcher.matched_instructions));

          auto replace = matcher.get_replacements();
          for (const auto& r : replace) {
            TRACE(PEEPHOLE, 8, "-- %s\n", SHOW(r));
          }

          m_stats_inserted += replace.size();
          m_stats_removed += matcher.match_index;

          inserts.emplace_back(mei.insn, replace);
          matcher.reset();
          break; // Matched. No need to check other patterns.
        }
      }
    }

    for (auto& pair : inserts) {
      std::vector<IRInstruction*> vec{begin(pair.second), end(pair.second)};
      transform->insert_after(pair.first, vec);
    }
    for (auto& insn : deletes) {
      transform->remove_opcode(insn);
    }
  }

  void print_stats() {
    TRACE(PEEPHOLE, 1, "%d instructions removed\n", m_stats_removed);
    TRACE(PEEPHOLE, 1, "%d instructions inserted\n", m_stats_inserted);
    TRACE(PEEPHOLE,
          1,
          "%d net instruction change\n",
          m_stats_inserted - m_stats_removed);
    TRACE(PEEPHOLE,
          1,
          "%lu patterns matched and replaced\n",
          std::accumulate(begin(m_matchers_stat), end(m_matchers_stat), 0L));
    TRACE(PEEPHOLE, 5, "Detailed pattern match stats:\n");
    for (size_t i = 0; i < m_matchers_stat.size(); ++i) {
      TRACE(PEEPHOLE,
            5,
            "%s: %d\n",
            m_matchers[i].pattern.name.c_str(),
            m_matchers_stat[i]);
    }
  }

  void run() {
    walk_methods(m_scope, [&](DexMethod* m) {
      if (m->get_code()) {
        peephole(m);
      }
    });

    print_stats();
  }
};
}

void PeepholePassV2::run_pass(DexStoresVector& stores,
                              ConfigFiles& /*cfg*/,
                              PassManager& mgr) {
  auto scope = build_class_scope(stores);
  PeepholeOptimizerV2(scope, config.disabled_peepholes).run();
  if (!contains<std::string>(config.disabled_peepholes,
                             RedundantCheckCastRemover::get_name())) {
    RedundantCheckCastRemover(mgr, scope).run();
  } else {
    TRACE(PEEPHOLE,
          2,
          "not running disabled peephole opt %s\n",
          RedundantCheckCastRemover::get_name().c_str());
  }
}

static PeepholePassV2 s_pass;
