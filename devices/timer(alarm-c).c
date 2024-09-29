#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

// 8254 (PIT) 칩을 사용하여 일정 주기마다 하드웨어 인터럽트 발생.
// 인터럽트로 스케줄링, CPU 시간 측정 등의 기능 수행

// 주파수는 19 ~ 1000 Hz 사이. 범위 벗어나면 에러
#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

// OS가 부팅된 이후로 발생한 타이머 틱의 총 수
static int64_t ticks;

// 타이머 틱당 루프의 수. timer_calibrate() 함수에 의해 초기화
static unsigned loops_per_tick;

// 인터럽트 핸들러 함수 및 여러 내부에서만 사용되는 함수 정의
static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

// PIT를 설정하여 TIMER_FREQ의 주기로 인터럽트를 발생시키도록 함
void
timer_init (void) {
	// 기본 클럭속도를 TIMER_FREQ로 나누어 인터럽트 주기 결정
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;	// 1193180 : PIT 칩의 기본 클럭 속도

	// outb : 하드웨어에 명령을 전달하는 함수, 타이머의 제어 워드 및 타이머 주기 설정
	outb (0x43, 0x34);    			// Control Word: 카운터 0, LSB 먼저, 모드 2, 이진드.
	outb (0x40, count & 0xff);	// 주파수의 하위 바이트 설정
	outb (0x40, count >> 8);		// 주파수의 상위 바이트 설정

	// 0x20 인터럽트 백터에 timer_interrupt 핸들러를 등록하여 타이머 인터럽트 발생 시 처리
	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

// CPU 성능에 따라 1틱 동안 몇 회 루프할 수 있는지 측정하여 loops_per_tick 값을 설정.
// 이후 busy_wait와 같은 짧은 지연에서 정확한 타이밍을 제공하는 데 사용됨
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	// 인터럽트가 활성화된 상태인지 확인
	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	// 루프 카운터를 초기화하여 1 틱 동안 실행될 수 있는 최대 값을 찾기 시작
	loops_per_tick = 1u << 10;
	// 현재 설정된 루프 카운터로 1 틱 이상 시간이 걸리면 더 이상 증가하지 않음
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		// 오버플로가 발생하지 않도록 확인
		ASSERT (loops_per_tick != 0);
	}

	// 더 작은 비트 단위로 미세하게 조정
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

// OS가 부팅된 이후 발생한 총 타이머 틱 수를 반환.
// 인터럽트를 비활성화하여 ticks 값을 안전하게 읽음
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();	// intr_disable : 현재 인터럽 비활성, 이전 인터럽 반환
	int64_t t = ticks;
	// 원래 인터럽트 상태 복원
	intr_set_level (old_level);
	barrier ();	// 컴파일러가 코드 최적화를 통해 메모리 접근 순서를 변경하지 않도록 강제
	return t;
}

// 주어진 시간(틱) 동안 경과된 타이머 틱 수를 반환
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

// 주어진 틱만큼 실행을 중단하고, 그 동안 CPU를 양보.
// 인터럽트가 활성화된 상태에서만 실행되도록 보장
void
timer_sleep (int64_t ticks) {
	int64_t start = timer_ticks ();	// timer_ticks : 시작 시점의 틱 기록

	ASSERT (intr_get_level () == INTR_ON);	// 인터럽트 활성화 됨?
	/* busy_wait 방식
	while (timer_elapsed (start) < ticks)		// 지정된 틱 수만큼 대기
		thread_yield ();	// 다른 쓰레드에 CPU를 양보
		*/
	thread_sleep(start + ticks);	// 현재 시각(start) + 잠들 시간(ticks)
} 

// 주어진 밀리초(ms) 만큼 실행을 중단
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

// 주어진 마이크로초(us) 만큼 실행을 중단
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

// 주어진 나노초(ns) 만큼 실행을 중단
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

// 현재까지 경과한 타이머 틱 수를 출력
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

// 타이머 인터럽트 핸들러. 
// 타이머 틱 수를 증가시키고, 스레드 스케줄링을 처리
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;	// 틱 수 증가
	thread_tick ();	// 스케줄링 관련 함수 호출
	thread_wakeup (ticks);
}

// 주어진 루프 수가 1 틱 이상 걸리는지 확인
// 1 틱 동안 기다린 후, 주어진 루프 수만큼 루프를 돌면서 1 틱이 지났는지 확인
static bool
too_many_loops (unsigned loops) {
	int64_t start = ticks;	// 시작 시점 기록
	while (ticks == start)	// 새로운 틱이 발생할 때까지 대기
		barrier ();

	start = ticks;
	busy_wait (loops);	// 주어진 루프 수만큼 대기
	barrier ();
	// 루프가 끝난 시점에서 틱이 변경되었는지 확인
	// 변경되었으면 true를 반환하여 루프 수가 너무 많다는 것을 알림
	return start != ticks;
}

// busy wait 구현 : 타이머 인터럽트 없이 정확한 짧은 시간을 대기해야 할 때 사용
// 루프 수만큼 대기하면서 시간을 소비하는 바쁜 대기 함수.
// 인라인되지 않도록 설정하여 코드 정렬이 타이밍에 영향을 미치지 않도록 함
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)	// 주어진 루프 수만큼 반복
		barrier ();				// 최적화 방지
}

// 주어진 시간(num/denom 초) 만큼 대기.
// 타이머 틱으로 변환 후 정확하게 대기하거나,
// 틱으로 변환되지 않는 짧은 시간일 경우 바쁜 대기를 사용하여 대기.
static void
real_time_sleep (int64_t num, int32_t denom) {
	// 주어진 시간(초)을 타이머 틱으로 변환
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		// 타이머 틱 이상 대기할 경우 timer_sleep()을 사용하여 CPU를 양보하면서 대기
		timer_sleep (ticks);
	} else {
		// 1 틱 이하의 짧은 시간일 경우 busy_wait를 사용하여 더 정확한 대기를 수행
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
