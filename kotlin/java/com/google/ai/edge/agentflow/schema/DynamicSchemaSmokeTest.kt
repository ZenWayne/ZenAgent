package com.google.ai.edge.agentflow.schema

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DynamicSchemaSmokeTest {
  @Test fun `proto compiled in kotlin is loadable and resolvable in native registry`() {
    DynamicSchema().use { schema ->
      schema.load(mapOf("dyntest/person.proto" to """
        syntax = "proto3";
        package dyntest;
        message Person { string name = 1; int32 age = 2; }
      """.trimIndent()))
      assertTrue(schema.hasType("dyntest.Person"))
      assertFalse(schema.hasType("dyntest.Ghost"))
    }
  }
}
