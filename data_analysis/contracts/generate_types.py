"""Generate TypeScript handoff types from the actual OpenAPI; no node install."""

import argparse
import json
from pathlib import Path
import re


def identifier(value):
    return re.sub(r"[^A-Za-z0-9_]", "_", value)


def ts(schema):
    if "$ref" in schema:
        return identifier(schema["$ref"].rsplit("/", 1)[-1])
    if "const" in schema:
        return json.dumps(schema["const"], ensure_ascii=False)
    if "enum" in schema:
        return " | ".join(json.dumps(value, ensure_ascii=False) for value in schema["enum"])
    for union in ("anyOf", "oneOf"):
        if union in schema:
            return "(" + " | ".join(ts(item) for item in schema[union]) + ")"
    if "allOf" in schema:
        return "(" + " & ".join(ts(item) for item in schema["allOf"]) + ")"
    kind = schema.get("type")
    if kind == "array":
        return "Array<" + ts(schema.get("items", {})) + ">"
    if kind == "object" or "properties" in schema:
        required = schema.get("required", [])
        fields = [json.dumps(name) + ("" if name in required else "?") + ": " + ts(value)
                  for name, value in schema.get("properties", {}).items()]
        if not fields:
            additional = schema.get("additionalProperties", True)
            return "Record<string, " + (ts(additional) if isinstance(additional, dict) else "unknown") + ">"
        return "{ " + "; ".join(fields) + " }"
    return {"string": "string", "integer": "number", "number": "number", "boolean": "boolean",
            "null": "null"}.get(kind, "unknown")


def generate(schema):
    lines = ["// Generated from contracts/openapi.json. Regenerate; do not hand-edit.",
             "// Amounts use integer CNY cents, energy Wh; date ranges are [startDate,endDate).", ""]
    for name, value in sorted(schema.get("components", {}).get("schemas", {}).items()):
        lines.append("export type " + identifier(name) + " = " + ts(value) + ";")
    for path, operations in sorted(schema["paths"].items()):
        for verb, operation in operations.items():
            if verb not in {"get", "post", "put", "delete", "patch"}:
                continue
            name = identifier(operation["operationId"])
            params = [param for param in operation.get("parameters", []) if param["in"] == "query"]
            query = {"type": "object", "properties": {p["name"]: p["schema"] for p in params},
                     "required": [p["name"] for p in params if p.get("required")]}
            lines += ["", "// " + verb.upper() + " " + path,
                      "export type " + name + "Query = " + ts(query) + ";"]
            body = operation.get("requestBody", {}).get("content", {}).get("application/json", {}).get("schema")
            if body:
                lines.append("export type " + name + "Body = " + ts(body) + ";")
            for status, response in operation["responses"].items():
                content = response.get("content", {}).get("application/json", {}).get("schema")
                if content:
                    lines.append("export type " + name + "Response" + status + " = " + ts(content) + ";")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", default=str(Path(__file__).with_name("openapi.json")))
    parser.add_argument("--output", required=True)
    parser.add_argument("--check", action="store_true", help="Verify the existing types without writing")
    args = parser.parse_args(argv)
    output = Path(args.output)
    generated = generate(json.loads(Path(args.input).read_text(encoding="utf-8")))
    if args.check:
        if not output.is_file() or output.read_text(encoding="utf-8") != generated:
            parser.exit(1, "TypeScript contract is missing or out of date\n")
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(generated)


if __name__ == "__main__":
    main()
