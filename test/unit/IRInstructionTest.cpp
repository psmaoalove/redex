/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "OpcodeList.h"
#include "Show.h"

// for nicer gtest error messages
std::ostream& operator<<(std::ostream& os, const DexInstruction& to_show) {
  return os << show(&to_show);
}

TEST(IRInstruction, RoundTrip) {
  g_redex = new RedexContext();

  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexField* field = DexField::make_field(ty, str, ty);
  DexMethod* method = DexMethod::make_method(
      ty, str, DexProto::make_proto(ty, DexTypeList::make_type_list({})));

  for (DexOpcode op : all_opcodes) {
    auto insn = DexInstruction::make_instruction(op);
    // populate the instruction args with non-zero values so we can check
    // if we have copied everything correctly
    if (insn->dests_size()) {
      insn->set_dest(0xf);
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      insn->set_src(i, i + 1);
    }
    if (insn->has_offset()) {
      insn->set_offset(0xf);
    }
    if (insn->has_literal()) {
      insn->set_literal(0xface);
    }
    if (insn->has_range()) {
      insn->set_range_base(0xf);
      insn->set_range_size(0xf);
    }
    if (insn->has_arg_word_count()) {
      insn->set_arg_word_count(5);
    }
    if (insn->has_strings()) {
      static_cast<DexOpcodeString*>(insn)->rewrite_string(str);
    } else if (insn->has_types()) {
      static_cast<DexOpcodeType*>(insn)->rewrite_type(ty);
    } else if (insn->has_fields()) {
      static_cast<DexOpcodeField*>(insn)->rewrite_field(field);
    } else if (insn->has_methods()) {
      static_cast<DexOpcodeMethod*>(insn)->rewrite_method(method);
    }
    EXPECT_EQ(*IRInstruction::make(insn)->to_dex_instruction(), *insn)
        << "at " << show(op);

    delete insn;
  }

  delete g_redex;
}

TEST(IRInstruction, TwoAddr) {
  using namespace dex_asm;
  // check that we recognize IRInstructions that can be converted to 2addr
  // from
  DexInstruction add_int_2addr(OPCODE_ADD_INT_2ADDR);
  add_int_2addr.set_dest(0);
  add_int_2addr.set_src(1, 1);
  EXPECT_EQ(*(dasm(OPCODE_ADD_INT, {0_v, 0_v, 1_v})->to_dex_instruction()),
            add_int_2addr);
  // IRInstructions that have registers beyond 4 bits can't benefit, however
  DexInstruction add_int_1(OPCODE_ADD_INT);
  add_int_1.set_dest(17);
  add_int_1.set_src(0, 17);
  add_int_1.set_src(1, 1);
  EXPECT_EQ(*(dasm(OPCODE_ADD_INT, {17_v, 17_v, 1_v})->to_dex_instruction()),
            add_int_1);
  DexInstruction add_int_2(OPCODE_ADD_INT);
  add_int_2.set_dest(0);
  add_int_2.set_src(0, 0);
  add_int_2.set_src(1, 17);
  EXPECT_EQ(*(dasm(OPCODE_ADD_INT, {0_v, 0_v, 17_v})->to_dex_instruction()),
            add_int_2);
}
