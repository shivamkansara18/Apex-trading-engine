# Pre-optimization journal

`preoptimization-balanced.apex` is a sealed version-1 journal captured from the
Phase 7 engine before Phase 8 representation/traversal changes. Its input is
`examples/workloads/balanced.workload`, with six default participant registrations.

Expected replay: 106 input records, 100 commands, 272 events; final book hash
`690b04bb1ba2c371`. This binary fixture tests replay compatibility across the
optimization rather than regenerating expected events with the implementation
under test. Keep it unchanged when optimizing. The fresh policy-comparison tests
also compare byte-identical journals for all eight performance policies.
