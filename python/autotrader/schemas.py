"""Schema registry: topics.json + v1/*.schema.json, envelope and payload validation."""
from __future__ import annotations

import json
from fnmatch import fnmatchcase
from pathlib import Path
from typing import Any

from jsonschema import Draft7Validator


def find_project_root(start: Path | None = None) -> Path:
    p = (start or Path.cwd()).resolve()
    for cand in [p, *p.parents]:
        if (cand / "schemas" / "topics.json").exists():
            return cand
    raise FileNotFoundError("project root (schemas/topics.json) not found above " + str(p))


def subject_matches(filt: str, subject: str) -> bool:
    f, s = filt.split("."), subject.split(".")
    for i, tok in enumerate(f):
        if tok == ">":
            return len(s) > i
        if i >= len(s):
            return False
        if tok != "*" and tok != s[i]:
            return False
    return len(f) == len(s)


class SchemaRegistry:
    def __init__(self, schemas_dir: Path | None = None):
        self.dir = Path(schemas_dir) if schemas_dir else find_project_root() / "schemas"
        self.topics = json.loads((self.dir / "topics.json").read_text(encoding="utf-8"))
        self.streams = self.topics["streams"]
        self._envelope = Draft7Validator(json.loads((self.dir / "v1" / "envelope.schema.json").read_text(encoding="utf-8")))
        self._payload: dict[str, Draft7Validator] = {}
        self._filters: list[tuple[str, str]] = []
        for t in self.topics["topics"]:
            self._filters.append((t["subject"], t["schema"]))
            if t["schema"] not in self._payload:
                self._payload[t["schema"]] = Draft7Validator(json.loads((self.dir / t["schema"]).read_text(encoding="utf-8")))

    def schema_for_subject(self, subject: str) -> str | None:
        for filt, file in self._filters:
            if subject_matches(filt, subject):
                return file
        return None

    def validate(self, subject: str, envelope: dict[str, Any]) -> list[str]:
        errors = [f"envelope/{'/'.join(str(p) for p in e.path)}: {e.message}" for e in self._envelope.iter_errors(envelope)]
        file = self.schema_for_subject(subject)
        if file is None:
            errors.append(f"subject '{subject}' is not registered in topics.json")
            return errors
        if isinstance(envelope.get("payload"), dict):
            errors += [f"payload/{'/'.join(str(p) for p in e.path)}: {e.message}" for e in self._payload[file].iter_errors(envelope["payload"])]
        return errors

    def validate_or_raise(self, subject: str, envelope: dict[str, Any]) -> None:
        errs = self.validate(subject, envelope)
        if errs:
            raise ValueError(f"schema validation failed for {subject}:\n  " + "\n  ".join(errs))

    def stream_for_subject(self, subject: str) -> str | None:
        for s in self.streams:
            for filt in s["subjects"]:
                if subject_matches(filt, subject):
                    return s["name"]
        return None
