/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POSIX_PROCESS_TEST_LIFETIME_ALLOCATOR_H
#define POSIX_PROCESS_TEST_LIFETIME_ALLOCATOR_H

struct lifetime_allocator {
	struct uk_alloc allocator;
	size_t calls, live, fail_at, locked_frees;
	void *allocations[16];
};

static void *lifetime_record(struct lifetime_allocator *state, void *memory)
{
	if (!memory)
		return NULL;
	for (size_t i = 0; i < ARRAY_SIZE(state->allocations); i++) {
		if (state->allocations[i])
			continue;
		state->allocations[i] = memory;
		state->live++;
		return memory;
	}
	UK_CRASH("Lifetime test allocator record table exhausted\n");
}

static void lifetime_forget(struct lifetime_allocator *state, void *memory)
{
	for (size_t i = 0; i < ARRAY_SIZE(state->allocations); i++) {
		if (state->allocations[i] != memory)
			continue;
		UK_ASSERT(state->live);
		state->allocations[i] = NULL;
		state->live--;
		return;
	}
	UK_CRASH("Lifetime allocation freed by the wrong allocator\n");
}

static void *lifetime_malloc(struct uk_alloc *allocator, size_t size)
{
	struct lifetime_allocator *state =
		__containerof(allocator, struct lifetime_allocator, allocator);
	void *memory;

	if (++state->calls == state->fail_at)
		return NULL;
	memory = uk_malloc(uk_alloc_get_default(), size);
	return lifetime_record(state, memory);
}

static void *lifetime_calloc(struct uk_alloc *allocator, size_t count,
			     size_t size)
{
	struct lifetime_allocator *state =
		__containerof(allocator, struct lifetime_allocator, allocator);
	void *memory;

	if (++state->calls == state->fail_at)
		return NULL;
	memory = uk_calloc(uk_alloc_get_default(), count, size);
	return lifetime_record(state, memory);
}

static void lifetime_free(struct uk_alloc *allocator, void *memory)
{
	struct lifetime_allocator *state =
		__containerof(allocator, struct lifetime_allocator, allocator);

	if (!memory)
		return;
	if (uk_mutex_is_locked(&pprocess_mutex))
		state->locked_frees++;
	lifetime_forget(state, memory);
	uk_free(uk_alloc_get_default(), memory);
}

static void lifetime_allocator_init(struct lifetime_allocator *state)
{
	*state = (struct lifetime_allocator){.allocator = {
						     .malloc = lifetime_malloc,
						     .calloc = lifetime_calloc,
						     .free = lifetime_free,
					     }};
}

#endif
