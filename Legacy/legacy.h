/* Copyright 2023 Simon Willcocks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Server task, passed handle to pipe.
void __attribute__(( naked, noreturn )) serve_legacy_swis( uint32_t pipe );

// These routines are given default implementations in Legacy,
// weakly linked.
OSTask *do_OS_Module( svc_registers *regs );
OSTask *do_OS_ServiceCall( svc_registers *regs );
OSTask *run_module_swi( svc_registers *regs, int swi );

// Return true if the module providing the swi has handlers
// registered (even if the specific SWI is unknown).
bool handler_available( uint32_t swi );

// These are provided by Legacy/memory.c
void do_OS_ReadDynamicArea( svc_registers *legacy_regs );
void do_OS_DynamicArea( svc_registers *legacy_regs );
void do_OS_ChangeDynamicArea( svc_registers *legacy_regs );
void do_OS_Memory( svc_registers *regs );
void do_OS_ValidateAddress( svc_registers *regs );
void do_OS_AMBControl( svc_registers *regs );
