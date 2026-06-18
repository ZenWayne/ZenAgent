package agentflow.dsl

import agentflow.jni.NativeBridge
import java.util.concurrent.LinkedBlockingQueue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow

/**
 * Handle to a JSON-defined workflow loaded on the JVM. Each `run()` call
 * invokes the native bridge which loads + runs the workflow fresh; future
 * iterations will cache the parsed Workflow + share a Runner.
 */
class JsonWorkflow internal constructor(
    private val modelPath: String,
    private val json: String,
) {
    fun run(userQuery: String): String =
        NativeBridge.runJsonWorkflow(modelPath, json, userQuery)

    /**
     * Streaming run: [onToken] is invoked with each generated text delta as it
     * arrives; the full assistant reply is returned when the run completes.
     *
     * ```
     * wf.runStreaming("hello") { delta -> print(delta) }
     * ```
     */
    fun runStreaming(userQuery: String, onToken: (String) -> Unit): String =
        // cancelId 0 = no cancellation handle (blocking call, no external handle).
        NativeBridge.runJsonWorkflowStreaming(modelPath, json, userQuery, { delta ->
            onToken(delta)
        }, 0L)

    /**
     * Streaming run exposed as a cold [Flow] of text deltas.
     *
     * The native run is blocking (it drives its own event loop to completion),
     * so it runs on a dedicated daemon thread; each delta is forwarded into the
     * flow via `trySend`. The flow completes when the run finishes and fails if
     * the native call throws. Collect on whatever dispatcher you like:
     *
     * ```
     * wf.streamTokens("hello").collect { delta -> print(delta) }
     * ```
     *
     * Cooperative cancellation: cancelling the collector (or e.g. `take(n)`)
     * signals the native run via a cancel handle, which breaks the in-flight
     * engine request so the run stops promptly instead of finishing in the
     * background.
     */
    fun streamTokens(userQuery: String): Flow<String> = flow {
        // The native run is a long, BLOCKING call that delivers deltas via a
        // callback on its own internal thread and only returns at end-of-turn.
        // We run it on a dedicated daemon thread and hand deltas to the flow
        // collector through a plain blocking queue, then emit them from inside
        // this flow{} builder. When the run finishes, the worker enqueues a
        // DONE sentinel; the emit loop sees it, stops, and the flow{} block
        // returns — kotlinx's most basic, guaranteed completion signal (a plain
        // flow builder completes when its block returns).
        val cancelId = NativeBridge.nativeNewCancel()
        val queue = LinkedBlockingQueue<Item>()
        val thread = Thread {
            try {
                NativeBridge.runJsonWorkflowStreaming(modelPath, json, userQuery, {
                    delta ->
                    queue.put(Item.Delta(delta))
                }, cancelId)
                queue.put(Item.Done(null))
            } catch (t: Throwable) {
                queue.put(Item.Done(t))
            } finally {
                NativeBridge.nativeFreeCancel(cancelId)
            }
        }
        thread.isDaemon = true
        thread.start()
        // IMPORTANT: only signal native cancellation when the COLLECTOR cancels
        // mid-stream. On normal/error completion the native run has already
        // finished; calling nativeCancel afterwards re-enters the native cancel
        // path on a torn-down run+io_context and DEADLOCKS — the flow{} block
        // then never returns, so the collector never sees completion and the UI
        // hangs on "Running…" forever. CancellationException (and only that) is
        // the collector-cancelled signal; rethrow it after signalling native.
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
}

/**
 * Loads a workflow from raw JSON.
 *
 * Example:
 * ```
 * val wf = loadWorkflow("/path/to/model.litertlm", workflowJsonString)
 * println(wf.run("hello"))
 * ```
 */
fun loadWorkflow(modelPath: String, json: String): JsonWorkflow =
    JsonWorkflow(modelPath, json)
