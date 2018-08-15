//see LICENSE for license
#ifndef _RISCV_BOGUS_ACC_ROCC_H
#define _RISCV_BOGUS_ACC_ROCC_H

#include "rocc.h"
#include "mmu.h"
#include "PicosModel.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <queue>      

#define MAX_PICOS_TICKETS_ALLOWED 256

class delegate_t : public rocc_t
{
  std::queue<long int> queue;
  SWPicosModel picosModel;
  InstructionHandler handler;
public:
  delegate_t() : picosModel(SWPicosModel(256)), handler(InstructionHandler(picosModel)) {}

  const char* name() { return "delegate"; }

  /*
   * rocc_insn_t insn: variable that represents the instruction used for calling
   *                   RoCC accelerator. It is used for retrieving the exact action
   *                   requested - whether it was to (case 0) setup msg and hash
   *                   addresses or to (case 1) setup msg length and run hash op.
   */
  reg_t custom3(rocc_insn_t insn, reg_t xs1, reg_t xs2)
  {
    /*
      val funct         = io.cmd.bits.inst.funct
      val doSubmit      = funct === UInt(1)
      val doNFetchWork  = funct === UInt(2)
      val doRetire      = funct === UInt(3)
      val doPFetchWork  = funct === UInt(4)
      val doSubRequest  = funct === UInt(5)
      val doWorkRequest = funct === UInt(6)
     */
    long long unsigned result = 0;
    switch (insn.funct)
    {
        case 1:
          handler.step();
          handler.submissionInst(xs1);
          break;
        case 2:
          handler.step();
          result = handler.nanosIDFetchingInst();
          break;
        case 3:
          handler.step();
          handler.retirementInst(xs1);
          break;
        case 4:
          handler.step();
          result = handler.picosIDFetchingInst();
          break;
        case 5:
          handler.step();
          handler.subRequestInst(xs1);
          break;
        case 6:
          handler.step();
          result = handler.workRequestInst(0);
          break;
      default:
        illegal_instruction();
    }

//  return -1; // in all cases, the accelerator returns nothing
    // Value written to rd
    return result;
  }
};
REGISTER_EXTENSION(delegate, []() { return new delegate_t; })
#endif
