# C backend externalization: remaining semantic decisions

Audit of the C backend after the template and ABI-policy rounds. The original audit changed no compiler source. A subsequent bounded correctness patch now rejects unsupported IR/operators, rejects pointer-name/array-dimension capacity overflow and separates hexadecimal string escapes from following hex digits. The semantic record design below remains proposed, not implemented.

## Current boundary

`src/backend/c/backend_c.c` remains 1,883 lines. Fixed output strings, runtime wrapper bodies, operator expressions, scalar type spellings, libc name classes and struct-return type names are external. The adapter reads 12 numeric ABI/policy records. This is substantial output externalization, but the host C implementation still chooses the lowering and sequence of fragments for an IR instruction.

The existing template VM is a bounded text lookup/substitution engine. It does not yet select an IR lowering from semantic records. Replacing a literal with `get("key")` does not move the decision represented by the surrounding C branch into the package.

The front-end IR schema and a target lowering policy are different contracts. IR opcodes, token names and aux-field encodings may remain host-known schema. Which template, call convention, access type, stack effect or C declarator production implements a schema value should become a package record.

## Remaining inventory

| Area | Current evidence | Target-specific decisions still in C | Reusable host mechanism |
| --- | --- | --- | --- |
| Integer operations | `emit_binop`, lines 70–113 | Token-to-expression mapping; signedness/width alternatives; unsigned comparisons and shift casts; silent default expression | Bounded matching of normalized operation/width/sign facts and emission of one expression |
| IR lowering | Main instruction switch, lines 1033–1827 | Which variant each opcode and aux mask selects; load/store widths and fallback defaults; floating/complex conversions; no-op extensions; unary alternatives | Enumerating IR, exposing typed fields, matching finite predicates, checking references and emitting bounded sequences |
| Calls | Lines 1484–1675 | Special treatment of printf, puts, setjmp, indirect calls and other named callees; per-argument cast choice; integer/complex result shape; wrapper/user/libc arity; struct return placement | Symbol lookup, definition/prototype metadata, bounded argument iteration and deterministic joining |
| Type output | `emit_c_type`, lines 240–365 | Mapping type kind to production; qualifier placement; pointer-to-aggregate reference policy; anonymous pointer fallback; complex base choice | Bounded structural traversal with cycle detection and a declarator work stack |
| Attributes | Prototype lines 115–170 and body/global emission | Attribute-to-output selection; unsupported ARM keyword comments; host weak policy; section construction | Enumerating IR attribute facts and executing ordered attribute records |
| Stack analysis | Lines 375–565 | Opcode-specific stack effects and control-flow effects; call-result assumption; hard-coded opcode branches to extract labels | Label interning, graph traversal, checked max-depth analysis, worklist storage and resource limits |
| Frame planning | Lines 574–579; function setup around 1004–1027 | Recipe `align_up(max(local_size + padding, frame_min), align)` and fixed ABI compatibility contract | Checked addition/max/align-up over a small validated frame plan |
| Literal emission | Lines 30–60 and float/global initializer code | C quote delimiters, printable byte class, hexadecimal strategy, NaN/Inf spellings, choice of integer/floating literal representation | Bounded byte traversal and trusted fixed numeric-to-decimal conversion |
| Program layout | `c_emit`, lines 582 onwards | Ordering of runtime, constants, declarations, definitions, entry adapter; suppression of host libc globals; main naming policy | Iterating program collections, symbol deduplication, assembling named output sections |

Line numbers describe this audit snapshot and may shift in later changes. These are coverage locations, not counts of independent records.

## A finite record layer, not a general scripting language

Add a versioned semantic table alongside existing named text rules. Suggested v1 limits: at most 512 selection records, 8 predicates per record, 16 actions per record, 16 value slots, 16 loop argument entries, 64 type traversal levels and a fixed per-function action budget. Reject unsupported record versions, unknown fields, conflicting selections, missing required opcode coverage, unresolved references, arithmetic overflow and exhausted budgets. Do not permit recursion between selection records, arbitrary branches, memory reads, host calls, dynamically selected function names or embedded native/Wasm code.

The host constructs an explicit fact vector. Examples: `ir.op`, `ir.value`, `ir.aux`, normalized token operation, signedness, source/value width, complex flag, volatile flag, global category, callee class, result category, argument count and declared return size. Reading IR aux fields is schema decoding; package predicates select the lowering. A generic fact provider must not hide a C-specific lowering behind a fact such as `use_c_complex_special_case`.

Permitted predicates are equality, membership in a declared finite enum set, integer range, and masked equality. A masked predicate such as `aux & mask == value` must validate mask/value width, and `value` may not set bits outside `mask`. Ordering is explicit by priority; ambiguous records at the same priority fail validation. Unknown operations fail instead of falling back to a guessed expression.

Permitted actions are `bind` from approved fact slots, bounded integer `add/sub/and/shr/align_up`, `emit` of a referenced text rule with explicitly named argument sources, and `for_args` with a finite count source and a body drawn from the same bounded action list. Structured symbol/type/list iteration remains in the host. The package chooses a production; it cannot execute arbitrary expressions.

Illustrative syntax only:

```text
select binary_add
  match ir.op in [BINARY,64BINARY]
  match token.op = PLUS
  emit binary_prologue
  emit add_expression
  emit statement_end
end

select frame_load_complex
  match ir.op = PUSH_VAR
  match access.bytes = 16
  emit frame_load_complex [ir.value]
  stack pop=0 push=1
end

select frame_load_integer
  match ir.op = PUSH_VAR
  match access.bytes in [1,2,4,8]
  emit frame_load_integer [ir.value,access.bytes,access.unsigned]
  stack pop=0 push=1
end
```

These records change the choice of emitted production, rather than merely changing what an existing C-selected key prints. The first implementation can keep opcode dispatch in a generic indexed lookup; that dispatch must not contain a backend-specific switch selecting template keys.

## Proposed bounded implementation sequence

1. **Operation selection and whole-instruction templates.** Cover binary/unary operations, float/complex arithmetic and conversion, constants, local load/store, direct labels/jumps, memcpy, field accesses, returns and no-ops. Combine partial numeric fragments into whole-instruction templates with named binding records. Move width/sign alternatives from C `if` chains into selectors. Aim to remove most non-call arms of the large C switch. Keep asm text escaping as an explicit shared primitive.
2. **Call descriptor records.** Replace named-callee `if` chains and six partly overlapping name tables with one descriptor per callee or family: route (host/wrapper/user/indirect), argument source member, fixed/dynamic arity, cast mode, return category and optional special prologue/epilogue templates. Model printf argument classes through declared variadic metadata and a finite cast table. Describe setjmp/longjmp as explicit special call productions, not host functions called by the interpreter. User declarations supply return metadata; records select its lowering.
3. **Stack/control records.** Associate pop/push and branch target effects with the selected lowering. Then run a generic checked CFG analysis. This prevents drift between emission and the independent stack-effect switch. Preserve a bounded worklist; reject unsupported inconsistent stack paths rather than claim a capped linear scan is necessarily an overestimate.
4. **C declarator and attribute productions.** Provide finite production kinds (`scalar`, `pointer`, `array`, `aggregate_ref`, `aggregate_body`, `function_pointer`) and package mappings from type kinds/facts to production templates. A shared structural walker handles finite recursion and cycles. Move qualifier placement and anonymous aggregate fallback policy into explicit production records. Add attribute selector records with declared order.
5. **Frame recipe and layout handshake.** Use a finite frame recipe and the cross-stage ABI contract described below. Moving the recipe alone does not make an incompatible ABI editable.

Each phase should have same-Wasm-SHA tests that change a semantic selection record, demonstrate a different lowering, and run both generated programs. Preserve existing C language and error suites; add missing/ambiguous selectors and resource-limit failures. Do not merge phases by hiding the existing switch behind one host `lower_c_instruction` interpreter action.

## ABI output text and the fixed frontend contract

The current C backend descriptor uses `sizeof(void*)`, and the compiler initializes front-end pointer layout from it. In the Wasm build this means **32-bit pointers**. The generated C stack slot is still an integer/float/complex union with 8-byte integer slots; pointer width and integer slot width must remain separate facts. C `long` is a separate 4-byte front-end policy in the current compiler. An emitted host C translation unit can otherwise use a different host data model, so a successful simple arithmetic run does not prove aggregate/pointer ABI portability.

The numeric rules currently validate exact compatibility for pointer bits, slot bytes, call arities, frame alignment and the varargs/struct-return offsets. Those exact values are intentionally still present in the adapter's validator. This is a compatibility fence, not complete externalization of the frontend ABI. The frame/stack policies genuinely affect output, while changing a contract key alone is rejected.

A safe next interface is an `XhTargetLayout` loaded and validated **before parsing/type layout**, passed through semantic analysis and IR generation, encoded into staged-IR metadata, and checked again when the backend consumes the IR. Required fields include pointer/long/int widths and alignment, integer slot bytes, complex slot representation, call argument layout, hidden return convention and varargs access convention. Stage identity/fingerprints must include the semantic layout, not merely a template path.

Varargs and struct-return offsets currently also originate in IR generation and standard-header/runtime expectations. The rule engine cannot authorize changing them independently. Either:

- parameterize all producers and consumers from the same layout object; or
- declare a named layout contract version in the external package, validate that version against the compiled frontend, and retain exact supported-layout rejection.

The latter is a reasonable bounded intermediate result, but must be described as a fixed supported ABI. Runtime wrapper text must carry the same contract version. ABI scalar values duplicated as numeric text in generated wrappers need substitution from that layout or validation of the wrapper contract; otherwise editable numeric records and editable runtime bodies can disagree silently.

A finite frame recipe can use `local_bytes`, `padding`, named reserved-region end values and `align` as operands, with only checked `add`, `max` and `align_up`. Example formula: `align_up(max(local_bytes + padding, varargs.end, struct_return.end, frame_min), frame_align)`. Reserved regions need an explicit capacity and overlap check. `frame_min >= 8704` currently approximates these requirements with one exact minimum. It is not a general region-layout validator.

## Existing issues to account for, not silently preserve as shared algorithms

- The stack-effect table treats `IR_TRUNC` as pushing one slot while emitted truncation updates the existing top slot. `IR_CALL` assumes a result even when emission suppresses a direct void result, and complex intrinsic variants can have different effects from the opcode's usual form. Selection-linked effect records should express actual variants.
- The stack analysis caps iteration at 32, caps named labels at 256 and uses a linear fallback for excessive labels. A generic engine must distinguish a hard resource bound from a sound successful analysis. These paths should reject or use a justified conservative bound; the existing comment alone is not proof.
- Type pointer names and array dimensions retain fixed capacities. The subsequent bounded patch now rejects pointer-name overflow and more than 16 array dimensions. A shared declarator walker still needs a general depth/cycle budget.
- The subsequent patch splits C string tokens after a hexadecimal escape when the next literal byte is a hex digit. Generated-C execution verifies bytes AB,41. Literal-format policy still resides partly in C.
- The subsequent patch rejects unknown IR and unsupported integer binary, unary and comparison tokens with a diagnostic and no output. Semantic rule coverage should preserve that rejection unless the package explicitly declares a no-op with a checked stack effect.
- The 16 KB expansion scratch and VM action/file limits are implementation resource limits. Package policies cannot raise them without a checked allocation/budget plan, especially under approximately 4 MB RAM.

## Completion criteria for a subsequent implementation round

A package should own operation/variant selection, call lowering descriptors, type/attribute production mappings, stack/control effects, ABI layout/contract declaration and frame recipes. The host may retain finite parsing, validation, matching, checked arithmetic, structural iteration, graph algorithms and safe numeric/string primitives. Documentation must identify which shared mechanisms are actually shared and which are merely C-specific algorithms moved into a differently named source file.

Until those semantic records and the cross-stage layout contract exist, the accurate status is: fixed emission templates, runtime text, ABI vocabulary and bounded sizing policies are external; substantial C-specific lowering selection and frontend-coupled ABI behavior remain compiled into the host.
