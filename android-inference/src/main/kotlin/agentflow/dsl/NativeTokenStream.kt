package agentflow.dsl

import agentflow.jni.NativeBridge
import agentflow.jni.TokenCallback
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.channels.trySendBlocking
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow

/**
 * Bridges a blocking, callback-based native streaming call into a cold
 * [Flow] of text deltas. The caller supplies only [run]: it must kick off the
 * blocking native call with the given `onToken` callback and `cancelId`, and
 * return the full reply when the run completes.
 *
 * The native run is BLOCKING: it delivers deltas via `onToken` on its own
 * internal thread and only returns at end-of-turn. We run it on a dedicated
 * daemon thread; [callbackFlow] supplies the buffer and the completion/cancel
 * lifecycle. `trySendBlocking` applies natural back-pressure — if the collector
 * falls behind, the worker (and thus native generation) blocks until it drains,
 * rather than buffering without bound.
 *
 * Resource ownership (this is what keeps it deadlock-free — see ZenAgent#24):
 *  - `nativeFreeCancel` is an idempotent RELEASE, tied to the worker's lifetime;
 *    it always runs once in the worker's `finally`.
 *  - `nativeCancel` is a SIGNAL to an in-flight run, not a release. [awaitClose]
 *    runs on EVERY termination, so it is gated on `done`: sent ONLY when the
 *    collector cancels while the worker is still running. Never after the worker
 *    has finished — re-entering the cancel path on a torn-down run/io_context
 *    DEADLOCKS, leaving the UI stuck on "Running…" forever.
 */
internal fun nativeTokenStream(
    run: (onToken: TokenCallback, cancelId: Long) -> String,
): Flow<String> = callbackFlow {
    val cancelId = NativeBridge.nativeNewCancel()
    val done = AtomicBoolean(false)

    Thread {
        var cause: Throwable? = null
        try {
            run({ delta -> trySendBlocking(delta) }, cancelId)
        } catch (t: Throwable) {
            cause = t
        } finally {
            // Mark finished BEFORE closing, so awaitClose can distinguish a
            // worker-driven close from a collector cancellation.
            done.set(true)
            NativeBridge.nativeFreeCancel(cancelId)
            close(cause)
        }
    }.apply {
        isDaemon = true
        name = "native-token-stream"
        start()
    }

    awaitClose {
        if (!done.get()) NativeBridge.nativeCancel(cancelId)
    }
}
