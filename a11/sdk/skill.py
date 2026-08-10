import pathlib
from typing import Any

import pydantic
import yaml
from pydantic import BaseModel, Field

from a11 import Status, StatusCode


class SkillAsset(BaseModel):
    filename: str
    url: str


class Skill(BaseModel):
    name: str = Field(
        min_length=1,
        max_length=64,
        pattern=r"^[a-zA-Z0-9](?:[a-zA-Z0-9\-_]*[a-zA-Z0-9])?$",
        description=(
            "The name of the skill. Max 64 characters. Lowercase letters, "
            "numbers, and hyphens only. Must not start or end with a hyphen."
        ),
    )
    description: str = Field(
        min_length=1,
        max_length=1024,
        description=(
            "Max 1024 characters. Non-empty. Describes what the skill does "
            "and when to use it."
        ),
    )

    license: str | None = Field(
        default=None,
        description="License name or reference to a bundled license file.",
    )
    compatibility: str | None = Field(
        default=None,
        max_length=500,
        description=(
            "Max 500 characters. Indicates environment requirements"
            " (intended product, system packages, network access, etc.)."
        ),
    )
    metadata: dict[str, Any] | None = Field(
        default=None,
        description="Arbitrary key-value mapping for additional metadata.",
    )
    allowed_tools: list[str] | None = Field(
        default=None,
        description="Pre-approved tools the skill may use. (Experimental)",
    )

    body: str = Field(
        description=(
            "The main body of the skill, containing the skill's instructions in"
            " natural language."
        ),
    )
    assets: list[SkillAsset] | None = Field(
        default=None,
        description="List of assets associated with the skill.",
    )

    @staticmethod
    def from_text(content: str):
        content = content.strip()
        parts = content.split("\n---\n", 1)

        if len(parts) != 2:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Found fewer or more than 2 YAML documents. Expected"
                    " exactly two: one for frontmatter, one for the skill's"
                    " body."
                ),
            ).to_exception()

        try:
            frontmatter = yaml.safe_load(parts[0])
        except yaml.YAMLError as e:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Failed to parse frontmatter as YAML: {e}",
                details=[{"frontmatter": str(parts[0])}],
            ).to_exception()
        body = parts[1]

        allowed_keys = {
            "name",
            "description",
            "license",
            "compatibility",
            "metadata",
            "allowed-tools",
        }
        required_keys = {"name", "description"}
        actual_keys = set(frontmatter.keys())

        if not required_keys.issubset(actual_keys):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Frontmatter is missing required keys:"
                    f" {required_keys - actual_keys}"
                ),
            ).to_exception()

        if actual_keys - allowed_keys:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Frontmatter contains disallowed keys:"
                    f" {actual_keys - allowed_keys}"
                ),
            ).to_exception()

        allowed_tools: list[str] | None = None
        if "allowed-tools" in frontmatter:
            allowed_tools: list[str] = []
            for tool in frontmatter["allowed-tools"].split(" "):
                if not tool:
                    continue
                allowed_tools.append(tool)

        try:
            return Skill(
                name=frontmatter["name"],
                description=frontmatter["description"],
                license=frontmatter.get("license"),
                compatibility=frontmatter.get("compatibility"),
                metadata=frontmatter.get("metadata"),
                allowed_tools=allowed_tools,
                body=body,
            )
        except pydantic.ValidationError as exc:
            raise Status.from_exception(exc).to_exception()

    @staticmethod
    def from_file(path: str):
        with open(path, "r") as f:
            return Skill.from_text(f.read())

    @staticmethod
    def from_directory(path: str | pathlib.Path):
        path = pathlib.Path(path).expanduser().resolve()

        if not path.exists():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Path '{path}' does not exist.",
            ).to_exception()

        if not path.is_dir():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Path '{path}' is not a directory.",
            ).to_exception()

        skill_md_path = path / "SKILL.md"
        if not skill_md_path.exists():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"SKILL.md file does not exist in '{path}'.",
            ).to_exception()

        skill = Skill.from_text(skill_md_path.read_text())
        skill.fill_assets_from(path)
        return skill

    def fill_assets_from(self, path: str | pathlib.Path) -> "Skill":
        path = pathlib.Path(path)

        if not path.exists():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Assets path '{path}' does not exist.",
            ).to_exception()

        if not path.is_dir():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Assets path '{path}' is not a directory.",
            ).to_exception()

        self.assets: list[SkillAsset] = []
        for asset_path in path.rglob("*"):
            if asset_path.name == ".DS_Store":
                continue

            asset_path = asset_path.expanduser().resolve()

            if asset_path.is_file():
                relative_path = asset_path.relative_to(path)

                # Skip the SKILL.md file
                if str(relative_path).casefold() == "skill.md":
                    continue

                self.assets.append(
                    SkillAsset(
                        filename=str(relative_path),
                        url=f"file://{asset_path}",
                    )
                )

        return self

    def to_skill_md(self) -> str:
        skill_dict: dict[str, Any] = {
            "name": self.name,
            "description": self.description,
        }
        if self.license:
            skill_dict["license"] = self.license
        if self.compatibility:
            skill_dict["compatibility"] = self.compatibility
        if self.metadata is not None:
            skill_dict["metadata"] = self.metadata
        if self.allowed_tools:
            skill_dict["allowed-tools"] = " ".join(self.allowed_tools)

        try:
            yaml_str = yaml.dump(skill_dict)
        except Exception as e:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Failed to dump skill dict to YAML: {e}",
            ).to_exception()

        parts = [f"---\n{yaml_str}\n---\n", self.body]
        return "".join(parts)
