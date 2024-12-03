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

typedef struct {
} workspace_legacy;

typedef struct dynamic_area dynamic_area;

typedef struct {
  uint32_t queue;
  uint32_t owner;  // User task managing access to the legacy SVC stack
  // The SP when the task running legacy code was interrupted
  // If the task was in privileged mode at the time, there will be
  // a couple of words on the stack to restore the needed banked
  // registers.
  uint32_t *sp;

  uint32_t blocked_sp; // Blocked other than by an interrupt

  dynamic_area *dynamic_areas;
  uint32_t last_allocated_da;
  uint32_t last_da_top;
} shared_legacy;

