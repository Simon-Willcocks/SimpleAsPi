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

// Modern modules will use minimal svc stack and either return immediately
// or delegate the work to a helper task. They are also multi-processor
// aware (i.e. will lock out other cores in critical regions).

// Legacy modules might enable interrupts while in SVC.

typedef struct {
} workspace_module;

typedef struct module module;

typedef struct {
  module *modules;
  module *last;

  module *in_init;
} shared_module;

