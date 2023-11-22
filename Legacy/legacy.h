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

// Called once in svc32 mode (probably only temporary until modules are
// implemented, when this can be replaced by module init code).
void svc_pre_boot_sequence();

// Called in usr32 mode with no stack. (Ditto, RMRun.)
void __attribute__(( naked, noreturn )) boot_sequence();

// Server task, passed handle to pipe.
void __attribute__(( naked, noreturn )) serve_legacy_swis( uint32_t pipe );

// These routines are given default implementations in Legacy,
// weakly linked.
OSTask *do_OS_Module( svc_registers *regs );
bool needs_legacy_stack( uint32_t swi );
OSTask *run_module_swi( svc_registers *regs, int swi );
