package agentflow.dsl

import agentflow.jni.NativeBridge
import kotlinx.coroutines.flow.Flow

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
     * Streaming run exposed as a cold [Flow] of text deltas. Collect on whatever
     * dispatcher you like:
     *
     * ```
     * wf.streamTokens("hello").collect { delta -> print(delta) }
     * ```
     *
     * All the blocking-call / cancellation plumbing lives in [nativeTokenStream];
     * cancelling the collector signals the native run so it stops promptly.
     */
    fun streamTokens(userQuery: String): Flow<String> =
        nativeTokenStream { onToken, cancelId ->
            NativeBridge.runJsonWorkflowStreaming(modelPath, json, userQuery, onToken, cancelId)
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

/**
 * A PERSISTENT multi-turn chat session. Unlike [JsonWorkflow.streamTokens]
 * (which creates a fresh engine per message — reloading the model and tripping
 * the engine's per-process registration on the 2nd message), a session loads
 * the engine ONCE and reuses one conversation across messages, so the model
 * keeps dialogue history and second/third/… messages just work.
 *
 * Lifecycle:
 * ```
 * val sess = openChatSession(modelPath, workflowJson)  // loads model once (blocking)
 * sess.streamTokens("hi").collect { print(it) }        // turn 1
 * sess.streamTokens("and again?").collect { ... }      // turn 2 — same context
 * sess.close()                                          // free engine + worker thread
 * ```
 *
 * [openChatSession] blocks while the model loads; call it off the main thread.
 * Only one [streamTokens] turn runs at a time (the native side serializes).
 */
class ChatSession internal constructor(private val sessionId: Long) {

    /**
     * Streams one user message's reply deltas as a cold [Flow], reusing this
     * session's engine + conversation. Same blocking-call / cancellation
     * semantics as [JsonWorkflow.streamTokens] — see [nativeTokenStream].
     */
    fun streamTokens(userQuery: String): Flow<String> =
        nativeTokenStream { onToken, cancelId ->
            NativeBridge.nativeSessionSendMessage(sessionId, userQuery, onToken, cancelId)
        }

    /** Frees the native engine + worker thread. Safe to call once; idempotent-ish. */
    fun close() {
        NativeBridge.nativeCloseSession(sessionId)
    }
}

/**
 * Opens a persistent [ChatSession], loading the model ONCE. Blocks while the
 * engine loads (call off the main thread). Throws on failure.
 */
fun openChatSession(modelPath: String, workflowJson: String): ChatSession =
    ChatSession(NativeBridge.nativeCreateSession(modelPath, workflowJson))
