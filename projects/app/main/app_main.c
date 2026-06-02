// Copyright 2020-2025 Beken
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

#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>

static void hello_task(beken_thread_arg_t arg)
{
	(void)arg;
	int i = 1;

	while (1)
	{
		os_printf("hello-%d\r\n", i++);
		rtos_delay_milliseconds(10000);
	}
}

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);
extern int bk_cli_init(void);
extern void bk_set_jtag_mode(uint32_t cpu_id, uint32_t group_id);

void user_app_main(void)
{
	// beken_thread_t thd = NULL;
	// rtos_create_thread(&thd, BEKEN_APPLICATION_PRIORITY, "hello_task",
					//    (beken_thread_function_t)hello_task, 1024, NULL);
}

int main(void)
{
#if (CONFIG_SYS_CPU0)	//注释：如果是CPU0，则设置用户APP入口函数为user_app_main
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();

	return 0;
}