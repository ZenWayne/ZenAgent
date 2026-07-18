package agentflow.dsl

import agentflow.jni.NativeBridge
import agentflow.jni.TokenCallback
import java.util.concurrent.LinkedBlockingQueue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow

/**
 * Bridges a blocking, callback-based native streaming call into a cold
 * [Flow] of text deltas. The caller supplies only [run]: it must kick off the
 * blocking native call with the given `onToken` callback and `cancelId`, and
 * return the full reply when the run completes.
 *
 * All the plumbing — the daemon worker thread, the hand-off queue, the DONE
 * sentinel, the cancel signal and the native-handle release — lives here, in
 * one reusable place, so each streaming entry point stays a one-liner.
 *
 * The native run is BLOCKING: it delivers deltas via `onToken` on its own
 * internal thread and only returns at end-of-turn. We run it on a dedicated
 * daemon thread and hand deltas to the collector through a plain blocking
 * queue, then emit them from inside the flow{} builder. When the run finishes
 * the worker enqueues a DONE sentinel; the emit loop sees it, stops, and the
 * flow{} block returns — kotlinx's most basic, guaranteed completion signal.
 *
 * Resource ownership (this is what keeps it deadlock-free — see ZenAgent#24):
 *  - `nativeFreeCancel` is an idempotent RELEASE; it is tied to the worker's
 *    lifetime and always runs once in the worker's `finally`.
 *  - `nativeCancel` is a SIGNAL to an in-flight run, not a release. It is sent
 *    ONLY when the collector cancels mid-stream (`CancellationException`).
 *    Never on normal/error completion: by then the native run has finished and
 *    re-entering its cancel path on a torn-down run/io_context DEADLOCKS, which
 *    would leave the flow{} block hung and the UI stuck on "Running…" forever.
 */
internal fun nativeTokenStream(
    run: (onToken: TokenCallback, cancelId: Long) -> String,
): Flow<String> = flow {
    val cancelId = NativeBridge.nativeNewCancel()
    val queue = LinkedBlockingQueue<Item>()
    Thread {
        try {
            run({ delta -> queue.put(Item.Delta(delta)) }, cancelId)
            queue.put(Item.Done(null))
        } catch (t: Throwable) {
            queue.put(Item.Done(t))
        } finally {
            NativeBridge.nativeFreeCancel(cancelId)
        }
    }.apply {
        isDaemon = true
        name = "native-token-stream"
        start()
    }

    try {
        while (true) {
            when (val item = queue.take()) {
                is Item.Delta -> emit(item.text)
                is Item.Done -> {
                    item.error?.let { throw it }
                    break
                }
            }
        }
    } catch (c: CancellationException) {
        NativeBridge.nativeCancel(cancelId)
        throw c
    }
}

private sealed interface Item {
    data class Delta(val text: String) : Item
    data class Done(val error: Throwable?) : Item
}
