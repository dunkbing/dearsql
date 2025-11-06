#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
GENERATOR="${CMAKE_GENERATOR:-Unix Makefiles}"

POSTGRES_CONTAINER="${POSTGRES_CONTAINER:-dearsql-postgres-test}"
MYSQL_CONTAINER="${MYSQL_CONTAINER:-dearsql-mysql-test}"
POSTGRES_IMAGE="${POSTGRES_IMAGE:-postgres:15-alpine}"
MYSQL_IMAGE="${MYSQL_IMAGE:-mysql:8.0}"
POSTGRES_PORT="${POSTGRES_PORT:-55432}"
MYSQL_PORT="${MYSQL_PORT:-53306}"

POSTGRES_ENV_VARS=(
    "DEARSQL_TEST_PG_HOST=127.0.0.1"
    "DEARSQL_TEST_PG_PORT=${POSTGRES_PORT}"
    "DEARSQL_TEST_PG_DB=dearsql_test"
    "DEARSQL_TEST_PG_USER=postgres"
    "DEARSQL_TEST_PG_PASSWORD=postgres"
    "DEARSQL_TEST_PG_NAME=PostgresDocker"
)
MYSQL_ENV_VARS=(
    "DEARSQL_TEST_MYSQL_HOST=127.0.0.1"
    "DEARSQL_TEST_MYSQL_PORT=${MYSQL_PORT}"
    "DEARSQL_TEST_MYSQL_DB=dearsql_test"
    "DEARSQL_TEST_MYSQL_USER=dearsql"
    "DEARSQL_TEST_MYSQL_PASSWORD=dearsql"
    "DEARSQL_TEST_MYSQL_NAME=MySQLDocker"
)

declare -a STOP_CONTAINERS=()
declare -a REMOVE_CONTAINERS=()

function ensure_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: '$1' command not found. Please install it and retry." >&2
        exit 1
    fi
}

function container_running() {
    docker ps --format '{{.Names}}' | grep -qw "$1"
}

function container_exists() {
    docker ps -a --format '{{.Names}}' | grep -qw "$1"
}

function start_container() {
    local name="$1"
    local run_cmd="$2"

    if container_running "$name"; then
        echo "Container '$name' already running."
        return
    fi

    if container_exists "$name"; then
        echo "Starting existing container '$name'..."
        docker start "$name" >/dev/null
        STOP_CONTAINERS+=("$name")
    else
        echo "Launching new container '$name'..."
        eval "$run_cmd"
        STOP_CONTAINERS+=("$name")
        REMOVE_CONTAINERS+=("$name")
    fi
}

function wait_for_port() {
    local host="$1"
    local port="$2"
    local name="$3"
    local end_time=$((SECONDS + 60))
    while (( SECONDS < end_time )); do
        if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then
            exec 3>&-
            echo "${name} ready on ${host}:${port}"
            return 0
        fi
        sleep 0.5
    done
    echo "Timed out waiting for ${name} on ${host}:${port}" >&2
    return 1
}

function stop_containers() {
    if [[ "${KEEP_TEST_DB_CONTAINERS:-0}" == "1" ]]; then
        echo "KEEP_TEST_DB_CONTAINERS=1 set; leaving containers running."
        return
    fi

    for name in "${STOP_CONTAINERS[@]}"; do
        echo "Stopping container '$name'..."
        docker stop "$name" >/dev/null || true
    done

    for name in "${REMOVE_CONTAINERS[@]}"; do
        docker rm "$name" >/dev/null || true
    done
}

trap stop_containers EXIT

ensure_command docker
if ! docker info >/dev/null 2>&1; then
    echo "Error: unable to communicate with Docker daemon. Ensure Docker Desktop (or your Docker service) is running and you have permission to access it." >&2
    exit 1
fi

echo "Ensuring PostgreSQL test container is running..."
start_container "$POSTGRES_CONTAINER" \
    "docker run -d --name $POSTGRES_CONTAINER \
        -e POSTGRES_PASSWORD=postgres \
        -e POSTGRES_DB=dearsql_test \
        -p 127.0.0.1:${POSTGRES_PORT}:5432 \
        $POSTGRES_IMAGE >/dev/null"
wait_for_port "127.0.0.1" "$POSTGRES_PORT" "PostgreSQL"

echo "Ensuring MySQL test container is running..."
start_container "$MYSQL_CONTAINER" \
    "docker run -d --name $MYSQL_CONTAINER \
        -e MYSQL_ROOT_PASSWORD=mysql \
        -e MYSQL_DATABASE=dearsql_test \
        -e MYSQL_USER=dearsql \
        -e MYSQL_PASSWORD=dearsql \
        -p 127.0.0.1:${MYSQL_PORT}:3306 \
        $MYSQL_IMAGE >/dev/null"
wait_for_port "127.0.0.1" "$MYSQL_PORT" "MySQL"

echo "Configuring CMake project (generator: ${GENERATOR})..."
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "$GENERATOR"

echo "Building database_tests target..."
cmake --build "$BUILD_DIR" --target database_tests

echo "Running database tests..."
(
    cd "$BUILD_DIR"
    env "${POSTGRES_ENV_VARS[@]}" "${MYSQL_ENV_VARS[@]}" \
        ctest --output-on-failure --tests-regex database_tests "$@"
)

echo "All tests completed successfully."
