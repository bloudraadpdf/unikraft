/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/file.h>
#include <uk/test.h>

struct release_order {
	struct uk_file file;
	uk_file_refcnt references;
	unsigned int events[3];
	unsigned int count;
};

static void release_file(const struct uk_file *file, int what)
{
	struct release_order *order =
		__containerof(file, struct release_order, file);

	if (what & UK_FILE_RELEASE_RES) {
		order->events[order->count++] = 1;
		uk_file_release_weak(file);
		order->events[order->count++] = 3;
	}
	if (what & UK_FILE_RELEASE_OBJ)
		order->events[order->count++] = 2;
}

UK_TESTCASE(ukfile, release_resource_before_object)
{
	struct release_order order = {0};

	order.references = UK_FILE_REFCNT_INIT_VALUE(order.references);
	order.file = (struct uk_file){
		.refcnt = &order.references,
		._release = release_file
	};
	uk_file_acquire_weak(&order.file);
	uk_file_release(&order.file);
	UK_TEST_EXPECT_SNUM_EQ(order.count, 3);
	UK_TEST_EXPECT_SNUM_EQ(order.events[0], 1);
	UK_TEST_EXPECT_SNUM_EQ(order.events[1], 3);
	UK_TEST_EXPECT_SNUM_EQ(order.events[2], 2);
}

#if CONFIG_LIBUKFILE_FINALIZERS
struct cancel_finalizer {
	struct uk_file file;
	uk_file_refcnt references;
	struct uk_file_finalize_cb first;
	struct uk_file_finalize_cb second;
	unsigned int first_called;
	unsigned int second_called;
	unsigned int completed;
	unsigned int completed_in_first;
};

static void cancel_second(void *argument)
{
	struct cancel_finalizer *test = argument;

	test->first_called++;
	uk_file_finalizer_unregister(&test->file, &test->second);
	uk_file_finalizer_unregister(&test->file, &test->first);
	test->completed_in_first = test->completed;
}

static void finalizer_second(void *argument)
{
	struct cancel_finalizer *test = argument;

	test->second_called++;
}

static void finalizer_release(const struct uk_file *file __unused,
			      int what __unused)
{
}

static void finalizer_complete(void *argument)
{
	struct cancel_finalizer *test = argument;

	test->completed++;
}

static void cancel_test_init(struct cancel_finalizer *test)
{
	test->references = UK_FILE_REFCNT_INIT_VALUE(test->references);
	test->file = (struct uk_file){
		.refcnt = &test->references,
		._release = finalizer_release
	};
	test->first = (struct uk_file_finalize_cb){
		.fin = cancel_second,
		.complete = finalizer_complete,
		.arg = test
	};
	test->second = (struct uk_file_finalize_cb){
		.fin = finalizer_second,
		.complete = finalizer_complete,
		.arg = test
	};
	uk_file_finalizer_register(&test->file, &test->second);
	uk_file_finalizer_register(&test->file, &test->first);
}

UK_TESTCASE(ukfile, cancel_pending_finalizer)
{
	struct cancel_finalizer test = {0};

	cancel_test_init(&test);
	uk_file_release(&test.file);
	UK_TEST_EXPECT_SNUM_EQ(test.first_called, 1);
	UK_TEST_EXPECT_SNUM_EQ(test.second_called, 0);
	UK_TEST_EXPECT_SNUM_EQ(test.completed_in_first, 1);
	UK_TEST_EXPECT_SNUM_EQ(test.completed, 2);
}

UK_TESTCASE(ukfile, cancel_finalizer_before_release)
{
	struct cancel_finalizer test = {0};

	cancel_test_init(&test);
	uk_file_finalizer_unregister(&test.file, &test.second);
	UK_TEST_EXPECT_SNUM_EQ(test.completed, 1);
	uk_file_finalizer_unregister(&test.file, &test.second);
	UK_TEST_EXPECT_SNUM_EQ(test.completed, 1);
	uk_file_release(&test.file);
	UK_TEST_EXPECT_SNUM_EQ(test.first_called, 1);
	UK_TEST_EXPECT_SNUM_EQ(test.second_called, 0);
	UK_TEST_EXPECT_SNUM_EQ(test.completed_in_first, 1);
	UK_TEST_EXPECT_SNUM_EQ(test.completed, 2);
}
#endif

uk_testsuite_register(ukfile, NULL);
