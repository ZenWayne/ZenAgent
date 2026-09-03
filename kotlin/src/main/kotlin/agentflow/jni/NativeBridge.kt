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

    /** Allocates a cancel handle. Returns its opaque id (0 means "none"). */
    external fun nativeNewCancel(): Long

    /** Signals cancellation for a running streaming call. Any thread; no-op if freed. */
    external fun nativeCancel(cancelId: Long)

    /** Releases a cancel handle. Call after the run has finished. */
    external fun nativeFreeCancel(cancelId: Long)

    /**
     * Constrained tool-mode run. The main agent runs with constrained tool-call
     * decoding (no token stream — LiteRT-LM has no streaming constrained
     * variant); tool lifecycle events are delivered to [onEvent] live, and the
     * full assistant reply is returned when the run completes.
     *
     * [tools] are registered into the run's host tool registry; names declared
     * in the workflow JSON's `"tools"` must match, or the loader rejects them.
     * [signal] is the per-run [agentflow.dsl.CancellationSignal] flipped when
     * [cancelId] is cancelled.
     */
    external fun runJsonWorkflowConstrained(
        modelPath: String,
        workflowJson: String,
        tools: Array<agentflow.dsl.HostTool>,
        signal: agentflow.dsl.CancellationSignal,
        userQuery: String,
        onEvent: agentflow.dsl.RunEventCallback,
        cancelId: Long,
    ): String
}
