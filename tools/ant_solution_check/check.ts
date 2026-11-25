#!/usr/bin/env bun
// Client for the lite node's solution-check endpoints, built on https://github.com/qubic/qubic-typescript.
//
//   bun check.ts tree    --node URL --identity ID
//   bun check.ts tick    --node URL --tick N [--index I]
//   bun check.ts verify  --node URL --identity ID --parent-tick T --parent-index I [--anchor-tick A] [--nonce HEX]
//   bun check.ts recheck --node URL --tick T --index I
//
// verify and recheck sign with the guardian seed taken from the environment variable GUARDIAN_SEED.
//
// What a score request needs, and where each value comes from:
//
//   identity     The MINER's identity (the owner of the solution), not the guardian's.
//   parent       Run `tree` for the miner. Every stored solution has an address: `tick` and `index`.
//                A child names its parent by that address, so the parent's `tick` and `index` are the
//                request's parentTick and parentIndex. A first-generation solution has no stored parent:
//                use parentTick 0 and parentIndex 4294967295.
//   recordIndex  The solution's position in that node's store. /v1/ant-solutions starts its scan at
//                `recordIndexFrom`, skips the first `offset` matches and returns up to `limit`. It is never
//                part of a score request.
//   anchorTick   A recent tick the node still remembers the digest of. Without --anchor-tick the script
//                starts just below the node's current tick and steps back until the node accepts one.
//   nonce        32 bytes. Byte 0 is the algorithm tag (1). Byte 1 is `L | 0x20`, L from 1 to 10 (wiring
//                mode, top two bits zero). Byte 2 is the number of explore steps. The rest is free.
//                Without --nonce the script makes a random one.
//
// The signed message is 76 bytes: publicKey[32] | nonce[32] | anchorTick | parentTick | parentIndex, the
// three numbers as 32-bit little-endian. It is hashed with KangarooTwelve to 32 bytes and that digest is signed.

import { identityToPublicKey, k12, sign } from '@qubic.org/crypto'
import { toIdentity, toSeed } from '@qubic.org/types'

const ROOT_PARENT_TICK = 0
const ROOT_PARENT_INDEX = 4294967295
const ANCHOR_ATTEMPTS = 8

interface StoredSolution {
  recordIndex: number
  identity: string
  nonce: string
  tick: number
  index: number
  parentTick: number
  parentIndex: number
  anchorTick: number
  depth: number
  score: number
  shift: number
  networkHash: number
  networkHeld: boolean
  trustedAtCommit: boolean
}

interface ScoreRequest {
  identity: string
  nonce: Uint8Array
  anchorTick: number
  parentTick: number
  parentIndex: number
}

function fail(message: string): never {
  console.error(message)
  process.exit(1)
}

function parseOptions(words: string[]): Map<string, string> {
  const options = new Map<string, string>()
  for (let i = 0; i + 1 < words.length; i += 2) {
    if (!words[i].startsWith('--')) {
      fail(`expected an option, got "${words[i]}"`)
    }
    options.set(words[i].slice(2), words[i + 1])
  }
  return options
}

function required(options: Map<string, string>, name: string): string {
  return options.get(name) ?? fail(`missing --${name}`)
}

function requiredNumber(options: Map<string, string>, name: string): number {
  const value = Number(required(options, name))
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    fail(`--${name} must be a whole number that fits 32 bits`)
  }
  return value
}

function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('')
}

function fromHex(hex: string, byteCount: number): Uint8Array {
  if (!new RegExp(`^[0-9a-fA-F]{${2 * byteCount}}$`).test(hex)) {
    fail(`expected ${2 * byteCount} hex digits, got "${hex}"`)
  }
  return Uint8Array.from({ length: byteCount }, (_, i) => parseInt(hex.slice(2 * i, 2 * i + 2), 16))
}

async function getJson(url: string): Promise<{ httpStatus: number; body: any }> {
  const response = await fetch(url)
  const text = await response.text()
  try {
    return { httpStatus: response.status, body: JSON.parse(text) }
  } catch {
    return { httpStatus: response.status, body: { status: `not json: ${text.slice(0, 120)}` } }
  }
}

// Reads every page of /v1/ant-solutions: `nextRecordIndexFrom` of one reply is the `recordIndexFrom` of the next, 0 ends it.
async function listSolutions(node: string, query: Record<string, string | number>): Promise<{ solutions: StoredSolution[]; verifies: boolean }> {
  const solutions: StoredSolution[] = []
  let verifies = false
  let recordIndexFrom = 0
  do {
    const parameters = new URLSearchParams({ ...query, recordIndexFrom: String(recordIndexFrom) } as Record<string, string>)
    const { httpStatus, body } = await getJson(`${node}/v1/ant-solutions?${parameters}`)
    if (httpStatus !== 200) {
      fail(`/v1/ant-solutions answered HTTP ${httpStatus}: ${JSON.stringify(body)}`)
    }
    solutions.push(...body.solutions)
    verifies = body.nodeVerifiesScores
    recordIndexFrom = body.nextRecordIndexFrom
  } while (recordIndexFrom !== 0)
  return { solutions, verifies }
}

function printSolutions(solutions: StoredSolution[], verifies: boolean): void {
  console.log(`node verifies every score: ${verifies}`)
  console.table(
    solutions.map((solution) => ({
      recordIndex: solution.recordIndex,
      tick: solution.tick,
      index: solution.index,
      parentTick: solution.parentTick,
      parentIndex: solution.parentIndex,
      anchorTick: solution.anchorTick,
      depth: solution.depth,
      score: solution.score,
      shift: solution.shift,
      networkHeld: solution.networkHeld,
      trustedAtCommit: solution.trustedAtCommit,
    })),
  )
}

function randomAntNonce(): Uint8Array {
  const nonce = crypto.getRandomValues(new Uint8Array(32))
  const changesPerStep = 1 + (nonce[1] % 10)
  nonce[0] = 1
  nonce[1] = changesPerStep | 0x20
  nonce[2] = 0
  return nonce
}

function guardianSeed() {
  return toSeed(process.env.GUARDIAN_SEED ?? fail('set GUARDIAN_SEED to the guardian seed'))
}

function challengeBytes(request: ScoreRequest): Uint8Array {
  const message = new Uint8Array(76)
  message.set(identityToPublicKey(toIdentity(request.identity)), 0)
  message.set(request.nonce, 32)
  const numbers = new DataView(message.buffer)
  numbers.setUint32(64, request.anchorTick, true)
  numbers.setUint32(68, request.parentTick, true)
  numbers.setUint32(72, request.parentIndex, true)
  return message
}

async function requestScore(node: string, request: ScoreRequest): Promise<{ httpStatus: number; body: any }> {
  const digest = k12(challengeBytes(request), 32)
  const signature = await sign(digest, guardianSeed())
  const parameters = new URLSearchParams({
    identity: request.identity,
    nonce: toHex(request.nonce),
    anchorTick: String(request.anchorTick),
    parentTick: String(request.parentTick),
    parentIndex: String(request.parentIndex),
    signature: toHex(signature),
  })
  return getJson(`${node}/v1/ant-score?${parameters}`)
}

async function currentTick(node: string): Promise<number> {
  const { httpStatus, body } = await getJson(`${node}/live/v1/tick-info`)
  if (httpStatus !== 200) {
    fail(`/live/v1/tick-info answered HTTP ${httpStatus}`)
  }
  return body.tickInfo.tick
}

async function commandTree(options: Map<string, string>): Promise<void> {
  const { solutions, verifies } = await listSolutions(required(options, 'node'), { identity: required(options, 'identity') })
  printSolutions(solutions, verifies)
}

async function commandTick(options: Map<string, string>): Promise<void> {
  const query: Record<string, string | number> = { tick: requiredNumber(options, 'tick') }
  if (options.has('index')) {
    query.index = requiredNumber(options, 'index')
  }
  const { solutions, verifies } = await listSolutions(required(options, 'node'), query)
  printSolutions(solutions, verifies)
}

async function commandVerify(options: Map<string, string>): Promise<void> {
  const node = required(options, 'node')
  const request: ScoreRequest = {
    identity: required(options, 'identity'),
    nonce: options.has('nonce') ? fromHex(required(options, 'nonce'), 32) : randomAntNonce(),
    anchorTick: 0,
    parentTick: requiredNumber(options, 'parent-tick'),
    parentIndex: requiredNumber(options, 'parent-index'),
  }

  // The anchor tick is signed, so every candidate needs its own signature.
  const anchorCandidates = options.has('anchor-tick')
    ? [requiredNumber(options, 'anchor-tick')]
    : await currentTick(node).then((tick) => Array.from({ length: ANCHOR_ATTEMPTS }, (_, back) => tick - 1 - back))

  for (const anchorTick of anchorCandidates) {
    request.anchorTick = anchorTick
    const { httpStatus, body } = await requestScore(node, request)
    if (body.status === 'anchor-unavailable') {
      continue
    }
    console.log(`nonce ${toHex(request.nonce)} anchorTick ${anchorTick}`)
    console.log(`HTTP ${httpStatus} ${JSON.stringify(body)}`)
    return
  }
  fail(`the node holds no anchor digest for ticks ${anchorCandidates.join(', ')}`)
}

// Asks the node to score a solution it already stored and compares the answer with the stored record.
async function commandRecheck(options: Map<string, string>): Promise<void> {
  const node = required(options, 'node')
  const { solutions } = await listSolutions(node, { tick: requiredNumber(options, 'tick'), index: requiredNumber(options, 'index') })
  if (solutions.length !== 1) {
    fail('the node holds no solution at that tick and index')
  }
  const stored = solutions[0]

  const { httpStatus, body } = await requestScore(node, {
    identity: stored.identity,
    nonce: fromHex(stored.nonce, 32),
    anchorTick: stored.anchorTick,
    parentTick: stored.parentTick,
    parentIndex: stored.parentIndex,
  })
  console.log(`stored   score ${stored.score} shift ${stored.shift} trustedAtCommit ${stored.trustedAtCommit}`)
  console.log(`HTTP ${httpStatus} ${JSON.stringify(body)}`)
  if (body.status !== 'ok') {
    fail('the node did not score it')
  }
  if (body.score !== stored.score || body.shift !== stored.shift) {
    fail('MISMATCH: the node stored a score it does not compute')
  }
  console.log('match')
}

const commands: Record<string, (options: Map<string, string>) => Promise<void>> = {
  tree: commandTree,
  tick: commandTick,
  verify: commandVerify,
  recheck: commandRecheck,
}

const command = commands[process.argv[2] ?? ''] ?? fail('usage: bun check.ts tree|tick|verify|recheck --node URL ...')
await command(parseOptions(process.argv.slice(3)))
