package agentflow.jni

/**
 * Invoked once per generated text delta during a streaming run.
 *
 * `fun interface`, so callers can pass a lambda. The C++ side resolves
 * `onToken(String)` via JNI and invokes it directly off the io thread driving
 * the run — a direct streaming path that bypasses the trace-event stream.
 */
fun interface TokenCallback {
    fun onToken(token: String)
}

/**
 * Thin JNI surface. Loads libagentflow_jni.so on first reference and
 * forwards external calls into the C++ runtime.
 *
 * Object is internal — Kotlin callers should go through the DSL in
 * [agentflow.dsl] instead of calling these directly.
 */
internal object NativeBridge {
    init {
        System.loadLibrary("agentflow_jni")
    }

    /**
     * Synchronous one-agent run. Mirrors examples/agent-demo: build a
     * single-node graph, run the AgentNode's ReAct loop, return the
     * assistant_reply field as a UTF-8 string.
     *
     * Throws RuntimeException (from the C++ side) on engine creation or
     * inference errors.
     */
    external fun runAgent(
        modelPath: String,
        systemPrompt: String,
        userQuery: String,
        constrainedToolCalls: Boolean,
    ): String

    /**
     * Runs a JSON-defined workflow's main agent. MVP routes through the same
     * single-AgentNode pipeline as runAgent; multi-agent / sub-agent /
     * streaming are tracked as P16+.
     */
    external fun runJsonWorkflow(
        modelPath: String,
        workflowJson: String,
        userQuery: String,
    ): String

    /**
     * Streaming variant of [runJsonWorkflow]: the main agent runs in streaming
     * mode and each generated text delta is delivered to [onToken] as it
     * arrives. Returns the full assistant reply when the run completes.
     *
     * Streaming uses the unconstrained decoding path (the constrained C bridge
     * has no streaming variant), so this forces constrained tool calls off.
     */
    external fun runJsonWorkflowStreaming(
        modelPath: String,
        workflowJson: String,
        userQuery: String,
        onToken: TokenCallback,
        cancelId: Long,
    ): String

    /**
     * Creates a PERSISTENT multi-turn session: loads the engine (model) ONCE
     * and starts a worker thread. Returns an opaque session id (>0), or throws
     * on failure. Call [nativeSessionSendMessage] per user message to reuse the
     * same engine + conversation (the engine owns dialogue history server-side,
     * so messages continue the same conversation). Call [nativeCloseSession]
     * when done to free the engine and stop the worker thread.
     *
     * This avoids recreating the engine per message — which both reloads the
     * model (~GBs) and trips the engine factory's per-process registration.
     */
    external fun nativeCreateSession(modelPath: String, workflowJson: String): Long

    /**
     * Sends one user message on a persistent [sessionId], streaming reply deltas
     * to [onToken]. Reuses the session's engine + conversation (multi-turn).
     * Blocks until the turn completes, then returns the full assistant reply.
     */
    external fun nativeSessionSendMessage(
        sessionId: Long,
        userQuery: String,
        onToken: TokenCallback,
        cancelId: Long,
    ): String

    /** Tears down a persistent session (cancels in-flight decode, frees engine). */
    external fun nativeCloseSession(sessionId: Long)

    /** Allocates a cancel handle. Returns its opaque id (0 means "none"). */
    external fun nativeNewCancel(): Long

    /** Signals cancellation for a running streaming call. Any thread; no-op if freed. */
    external fun nativeCancel(cancelId: Long)

    /** Releases a cancel handle. Call after the run has finished. */
    external fun nativeFreeCancel(cancelId: Long)
}
