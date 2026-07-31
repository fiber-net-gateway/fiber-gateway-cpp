#!/usr/bin/env python3

"""Copy the Nacos configuration graph used by access-server into a test server."""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import pathlib
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass


DEFAULT_PROJECTS_DATA_ID = "ploto.unified-access.projects"
DEFAULT_ROUTE_PREFIX = "ploto.unified-access.route."
DEFAULT_ROUTE_GROUP = "ACCESS-SERVER"
DEFAULT_GRAY_DATA_ID = "ploto.unified-access.gray-match"
DEFAULT_GRAY_GROUP = "DEFAULT_GROUP"
SAFE_FILE_NAME = re.compile(r"[^A-Za-z0-9._-]")


class SyncError(RuntimeError):
    pass


@dataclass(frozen=True)
class ConfigEntry:
    data_id: str
    group: str
    content: bytes
    config_type: str
    kind: str
    project: str = ""


class NacosOpenApi:
    def __init__(self, base_url: str, username: str, password: str) -> None:
        self._base_url = base_url.rstrip("/")
        self._username = username
        self._password = password
        self._access_token = ""

    @staticmethod
    def _open(request: urllib.request.Request, label: str) -> bytes:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(request, timeout=30) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            body = error.read(512).decode("utf-8", errors="replace")
            raise SyncError(f"{label} returned HTTP {error.code}: {body}") from None
        except urllib.error.URLError as error:
            raise SyncError(f"{label} failed: {error.reason}") from None

    def login(self) -> None:
        if not self._username and not self._password:
            return
        if not self._username or not self._password:
            raise SyncError("Nacos username and password must both be set")
        body = urllib.parse.urlencode(
            {
                "username": self._username,
                "password": self._password,
            }
        ).encode()
        request = urllib.request.Request(
            f"{self._base_url}/v1/auth/users/login",
            data=body,
            method="POST",
        )
        payload = self._open(request, "Nacos login")
        try:
            parsed = json.loads(payload)
            self._access_token = parsed["accessToken"]
        except (KeyError, TypeError, ValueError):
            raise SyncError("Nacos login response did not contain accessToken") from None
        if not isinstance(self._access_token, str) or not self._access_token:
            raise SyncError("Nacos login returned an empty accessToken")

    def _auth_fields(self) -> dict[str, str]:
        if not self._access_token:
            return {}
        return {"accessToken": self._access_token}

    def get_config(self, data_id: str, group: str, tenant: str) -> bytes | None:
        query = {
            "dataId": data_id,
            "group": group,
            "tenant": tenant,
            **self._auth_fields(),
        }
        request = urllib.request.Request(
            f"{self._base_url}/v1/cs/configs?{urllib.parse.urlencode(query)}",
            method="GET",
        )
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(request, timeout=30) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            body = error.read(512).decode("utf-8", errors="replace")
            raise SyncError(
                f"read config dataId={data_id!r} group={group!r} returned "
                f"HTTP {error.code}: {body}"
            ) from None
        except urllib.error.URLError as error:
            raise SyncError(
                f"read config dataId={data_id!r} group={group!r} failed: {error.reason}"
            ) from None

    def publish_config(self, entry: ConfigEntry, tenant: str) -> None:
        try:
            content = entry.content.decode("utf-8")
        except UnicodeDecodeError:
            raise SyncError(f"config {entry.data_id!r} is not valid UTF-8") from None
        fields = {
            "dataId": entry.data_id,
            "group": entry.group,
            "tenant": tenant,
            "content": content,
            "type": entry.config_type,
            **self._auth_fields(),
        }
        request = urllib.request.Request(
            f"{self._base_url}/v1/cs/configs",
            data=urllib.parse.urlencode(fields).encode(),
            method="POST",
        )
        result = self._open(request, f"publish config {entry.data_id!r}")
        if result.strip().lower() != b"true":
            raise SyncError(f"publish config {entry.data_id!r} returned {result[:256]!r}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Dump the project-list, every referenced route config, and gray-match "
            "config used by access-server. Proxy use is disabled in-process."
        )
    )
    parser.add_argument(
        "--source-url",
        default=os.environ.get("ACCESS_SERVER_SOURCE_NACOS_URL", ""),
    )
    parser.add_argument(
        "--source-username",
        default=os.environ.get("ACCESS_SERVER_SOURCE_NACOS_USERNAME", ""),
    )
    parser.add_argument(
        "--source-password",
        default=os.environ.get("ACCESS_SERVER_SOURCE_NACOS_PASSWORD", ""),
    )
    parser.add_argument("--source-tenant", default="")
    parser.add_argument("--destination-url", default="")
    parser.add_argument(
        "--destination-username",
        default=os.environ.get("ACCESS_SERVER_DEST_NACOS_USERNAME", ""),
    )
    parser.add_argument(
        "--destination-password",
        default=os.environ.get("ACCESS_SERVER_DEST_NACOS_PASSWORD", ""),
    )
    parser.add_argument("--destination-tenant", default="")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--projects-data-id", default=DEFAULT_PROJECTS_DATA_ID)
    parser.add_argument("--route-prefix", default=DEFAULT_ROUTE_PREFIX)
    parser.add_argument("--route-group", default=DEFAULT_ROUTE_GROUP)
    parser.add_argument("--gray-data-id", default=DEFAULT_GRAY_DATA_ID)
    parser.add_argument("--gray-group", default=DEFAULT_GRAY_GROUP)
    parser.add_argument("--workers", type=int, default=12)
    args = parser.parse_args()
    if not args.source_url:
        parser.error(
            "--source-url or ACCESS_SERVER_SOURCE_NACOS_URL must be provided"
        )
    if args.workers < 1 or args.workers > 64:
        parser.error("--workers must be in range 1..64")
    return args


def prepare_output_dir(raw_path: str) -> pathlib.Path:
    path = pathlib.Path(raw_path)
    if path.exists():
        if not path.is_dir():
            raise SyncError(f"output path is not a directory: {path}")
        if any(path.iterdir()):
            raise SyncError(f"output directory is not empty: {path}")
        path.chmod(0o700)
    else:
        path.mkdir(mode=0o700, parents=True)
    routes = path / "routes"
    routes.mkdir(mode=0o700)
    return path


def parse_projects(content: bytes) -> list[str]:
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError:
        raise SyncError("project-list config is not valid UTF-8") from None
    text = text.strip()
    projects = text.split(";") if text else [""]
    while projects and not projects[-1]:
        projects.pop()
    seen: set[str] = set()
    unique: list[str] = []
    for project in projects:
        if project not in seen:
            seen.add(project)
            unique.append(project)
    return unique


def config_file_name(index: int, project: str) -> str:
    safe = SAFE_FILE_NAME.sub("_", project)
    if not safe or safe != project:
        digest = hashlib.sha256(project.encode()).hexdigest()[:12]
        safe = f"project-{digest}"
    return f"routes/{index:04d}-{safe}.json"


def write_private_bytes(path: pathlib.Path, content: bytes) -> None:
    path.write_bytes(content)
    path.chmod(0o600)


def write_dump(
    path: pathlib.Path,
    tenant: str,
    entries: list[ConfigEntry],
    missing_routes: list[str],
    missing_gray: bool,
) -> None:
    manifest_entries: list[dict[str, object]] = []
    route_index = 0
    for entry in entries:
        if entry.kind == "projects":
            file_name = "projects.txt"
        elif entry.kind == "gray":
            file_name = "gray-match.json"
        else:
            file_name = config_file_name(route_index, entry.project)
            route_index += 1
        write_private_bytes(path / file_name, entry.content)
        manifest_entries.append(
            {
                "kind": entry.kind,
                "project": entry.project or None,
                "dataId": entry.data_id,
                "group": entry.group,
                "type": entry.config_type,
                "file": file_name,
                "bytes": len(entry.content),
                "sha256": hashlib.sha256(entry.content).hexdigest(),
            }
        )
    manifest = {
        "format": 1,
        "sourceTenant": tenant,
        "createdAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "configs": manifest_entries,
        "missingRoutes": missing_routes,
        "missingGrayMatch": missing_gray,
    }
    manifest_path = path / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    manifest_path.chmod(0o600)


def main() -> int:
    args = parse_args()
    source = NacosOpenApi(args.source_url, args.source_username, args.source_password)
    source.login()

    project_content = source.get_config(
        args.projects_data_id,
        args.route_group,
        args.source_tenant,
    )
    if project_content is None:
        raise SyncError("source project-list config was not found")
    projects = parse_projects(project_content)
    entries = [
        ConfigEntry(
            data_id=args.projects_data_id,
            group=args.route_group,
            content=project_content,
            config_type="text",
            kind="projects",
        )
    ]

    def read_route(project: str) -> tuple[str, bytes | None]:
        return (
            project,
            source.get_config(
                f"{args.route_prefix}{project}",
                args.route_group,
                args.source_tenant,
            ),
        )

    project_order = {project: index for index, project in enumerate(projects)}
    route_results: list[tuple[str, bytes | None]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = [executor.submit(read_route, project) for project in projects]
        for future in concurrent.futures.as_completed(futures):
            route_results.append(future.result())
    route_results.sort(key=lambda item: project_order[item[0]])

    missing_routes: list[str] = []
    for project, content in route_results:
        if content is None:
            missing_routes.append(project)
            continue
        entries.append(
            ConfigEntry(
                data_id=f"{args.route_prefix}{project}",
                group=args.route_group,
                content=content,
                config_type="json",
                kind="route",
                project=project,
            )
        )

    gray_content = source.get_config(
        args.gray_data_id,
        args.gray_group,
        args.source_tenant,
    )
    if gray_content is not None:
        entries.append(
            ConfigEntry(
                data_id=args.gray_data_id,
                group=args.gray_group,
                content=gray_content,
                config_type="json",
                kind="gray",
            )
        )

    output_dir = prepare_output_dir(args.output_dir)
    write_dump(
        output_dir,
        args.source_tenant,
        entries,
        missing_routes,
        gray_content is None,
    )

    if args.destination_url:
        destination = NacosOpenApi(
            args.destination_url,
            args.destination_username,
            args.destination_password,
        )
        destination.login()
        for entry in entries:
            destination.publish_config(entry, args.destination_tenant)
        for entry in entries:
            copied = destination.get_config(
                entry.data_id,
                entry.group,
                args.destination_tenant,
            )
            if copied != entry.content:
                raise SyncError(f"verification mismatch for config {entry.data_id!r}")

    route_count = sum(1 for entry in entries if entry.kind == "route")
    gray_state = "present" if gray_content is not None else "missing"
    destination_state = "published-and-verified" if args.destination_url else "not-requested"
    print(f"projects={len(projects)}")
    print(f"routes={route_count}")
    print(f"missing_routes={len(missing_routes)}")
    print(f"gray_match={gray_state}")
    print(f"destination={destination_state}")
    print(f"dump={output_dir}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SyncError as error:
        print(f"sync failed: {error}", file=sys.stderr)
        sys.exit(1)
