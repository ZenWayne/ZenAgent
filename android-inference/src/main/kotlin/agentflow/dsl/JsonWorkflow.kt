package agentflow.dsl

import agentflow.jni.NativeBridge
import agentflow.jni.TokenCallback
import kotlinx.coroutines.flow.Flow

/**
 * Handle to a JSON-defined workflow loaded on the JVM.
 *
 * [streamTokens] — the chat entry point — is backed by a PERSISTENT
 * [ChatSession] opened lazily on the first turn and reused for every turn
 * afterwards. That is load-bearing, not an optimisation: the one-shot native
 * entry points ([run], [runStreaming]) build a brand-new `LiteRtLmEngine` per
 * call, and the engine factory's registration is per-PROCESS. The second
 * `Create` trips `ALREADY_EXISTS: Engine type already exists` and then blocks
 * forever inside the model load — the 2nd chat message would hang with the UI
 * stuck on "Running…" and not a single native log line. Reusing one session
 * also keeps dialogue history (the engine owns it server-side), so multi-turn
 * context just works.
 */
class JsonWorkflow internal constructor(
    private val modelPath: String,
    private val json: String,
) {
    // Opened on the first streamTokens turn, then reused. Guarded by `lock`
    // because the flow's worker thread — not the collector — opens it.
    private val lock = Any()
    private var session: ChatSession? = null

    private fun session(): ChatSession = synchronized(lock) {
        session ?: openChatSession(modelPath, json).also { session = it }
    }

    /**
     * One-shot run on a THROWAWAY engine. Safe only as the single inference
     * call in a process — see the class doc. Chat callers want [streamTokens].
     */
    fun run(userQuery: String): String =
        NativeBridge.runJsonWorkflow(modelPath, json, userQuery)

    /**
     * Streaming run on a THROWAWAY engine — same one-shot caveat as [run].
     * [onToken] is invoked with each generated text delta as it arrives; the
     * full assistant reply is returned when the run completes.
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
     * Streams one turn's text deltas as a cold [Flow], on this workflow's
     * persistent session (engine loaded ONCE, conversation reused, so turn N+1
     * sees turn N's context). Collect on whatever dispatcher you like:
     *
     * ```
     * wf.streamTokens("hello").collect { delta -> print(delta) }
     * wf.streamTokens("and again?").collect { delta -> print(delta) }  // same context
     * ```
     *
     * The first collection blocks its worker thread while the model loads.
     * Turns are serialized natively — one decode at a time. All the
     * blocking-call / cancellation plumbing lives in [nativeTokenStream];
     * cancelling the collector signals the native run so it stops promptly
     * WITHOUT tearing down the session, so the next turn still works.
     */
    fun streamTokens(userQuery: String): Flow<String> =
        nativeTokenStream { onToken, cancelId ->
            session().sendMessage(userQuery, onToken, cancelId)
        }

    /**
     * Frees the persistent session's engine + worker thread. The workflow stays
     * usable — the next [streamTokens] opens a fresh session (and reloads the
     * model), starting a new dialogue.
     */
    fun close() {
        synchronized(lock) {
            session?.close()
            session = null
        }
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
 * A PERSISTENT multi-turn chat session: the engine is loaded ONCE and one
 * conversation is reused across messages, so the model keeps dialogue history
 * and second/third/… messages just work. Contrast [JsonWorkflow.run] /
 * [JsonWorkflow.runStreaming], which build a throwaway engine per call and so
 * can only be used once per process.
 *
 * [JsonWorkflow.streamTokens] is built on this; use it directly when you want
 * to own the session lifetime yourself.
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
        nativeTokenStream { onToken, cancelId -> sendMessage(userQuery, onToken, cancelId) }

    /**
     * Blocking single-turn send used by the [Flow] builders. Delivers deltas to
     * [onToken] on the native worker thread and returns the full reply.
     */
    internal fun sendMessage(
        userQuery: String,
        onToken: TokenCallback,
        cancelId: Long,
    ): String = NativeBridge.nativeSessionSendMessage(sessionId, userQuery, onToken, cancelId)

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
