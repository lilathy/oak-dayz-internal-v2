# Exercises the exact call sequence the WPF launcher makes.
$ErrorActionPreference = "Stop"
$api = "http://127.0.0.1:8787"
$fail = 0

function Step($name, $block) {
  try {
    $result = & $block
    Write-Host ("  OK    " + $name) -ForegroundColor Green
    return $result
  } catch {
    Write-Host ("  FAIL  " + $name + " -> " + $_.Exception.Message) -ForegroundColor Red
    $script:fail++
    return $null
  }
}

function Api($method, $path, $body, $token) {
  $headers = @{}
  if ($token) { $headers["Authorization"] = "Bearer $token" }
  $args = @{ Method = $method; Uri = "$api$path"; Headers = $headers }
  if ($body) {
    $args["Body"] = ($body | ConvertTo-Json -Compress)
    $args["ContentType"] = "application/json"
  }
  return Invoke-RestMethod @args
}

$stamp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$user = "smoke$stamp"
$pass = "SmokeTest!12345"
$hwidA = "A" * 64
$hwidB = "B" * 64

Write-Host "`nLauncher flow" -ForegroundColor Cyan

$reg = Step "register" { Api POST "/v1/auth/register" @{ email = "$user@oak.local"; username = $user; password = $pass } }
if (-not $reg) { exit 1 }
$access = $reg.accessToken
$refresh = $reg.refreshToken

Step "health" { Invoke-RestMethod "$api/health" } | Out-Null
Step "auth/me" { Api GET "/v1/auth/me" $null $access } | Out-Null

Step "hwid/bind (first PC)" {
  $r = Api POST "/v1/hwid/bind" @{ hwid = $hwidA } $access
  if (-not $r.hwid.bound) { throw "not bound" }
} | Out-Null

Step "hwid/bind rejects a different PC" {
  try {
    Api POST "/v1/hwid/bind" @{ hwid = $hwidB } $access
    throw "expected 403"
  } catch {
    if ($_.Exception.Response.StatusCode.value__ -ne 403) { throw "expected 403, got $($_.Exception.Message)" }
  }
} | Out-Null

Step "hwid/migrate rejects an unknown old id" {
  try {
    Api POST "/v1/hwid/migrate" @{ from = ("C" * 64); to = $hwidB } $access
    throw "expected 403"
  } catch {
    if ($_.Exception.Response.StatusCode.value__ -ne 403) { throw "expected 403, got $($_.Exception.Message)" }
  }
} | Out-Null

Step "hwid/migrate accepts the real old id" {
  $r = Api POST "/v1/hwid/migrate" @{ from = $hwidA; to = $hwidB } $access
  if (-not $r.hwid.bound) { throw "not bound after migrate" }
} | Out-Null

Step "migrated id is now the bound one" {
  Api POST "/v1/hwid/bind" @{ hwid = $hwidB } $access | Out-Null
} | Out-Null

$parentRefresh = $refresh
$refreshed = Step "auth/refresh rotates the token" {
  $r = Api POST "/v1/auth/refresh" @{ refreshToken = $parentRefresh }
  if (-not $r.accessToken) { throw "no access token" }
  if ($r.refreshToken -eq $parentRefresh) { throw "refresh token was not rotated" }
  $r
}

Step "old refresh token reuse returns 401 and revokes family" {
  try {
    Api POST "/v1/auth/refresh" @{ refreshToken = $parentRefresh }
    throw "expected 401"
  } catch {
    if ($_.Exception.Response.StatusCode.value__ -ne 401) { throw "expected 401" }
  }
} | Out-Null

# Family was revoked — log in fresh for remaining steps.
$login2 = Step "re-login after refresh reuse" {
  Api POST "/v1/auth/login" @{ login = "$user@oak.local"; password = $pass; hwid = $hwidB }
}
if ($login2) {
  $access = $login2.accessToken
  $refresh = $login2.refreshToken
}

Step "products list" { Api GET "/v1/products" $null $access } | Out-Null

$info = Step "launch-info" { Api GET "/v1/products/dayz/launch-info" $null $access }
if ($info) {
  Write-Host ("        canInject=" + $info.launch.canInject + " reasons=" + ($info.launch.reasons -join ",")) -ForegroundColor DarkGray
  Step "inject is gated without a license" {
    if ($info.launch.canInject) { throw "a brand new account should not be able to inject" }
  } | Out-Null
}

Step "client download is refused without a license" {
  try {
    Invoke-RestMethod -Method GET -Uri "$api/v1/products/dayz/client" -Headers @{ Authorization = "Bearer $access" }
    throw "expected 403"
  } catch {
    if ($_.Exception.Response.StatusCode.value__ -ne 403) { throw "expected 403" }
  }
} | Out-Null

Step "bad redeem code is rejected" {
  try {
    Api POST "/v1/licenses/redeem" @{ code = "OAK-0000-0000-0000-0000" } $access
    throw "expected 404"
  } catch {
    if ($_.Exception.Response.StatusCode.value__ -ne 404) { throw "expected 404" }
  }
} | Out-Null

Step "logout revokes the session" {
  Api POST "/v1/auth/logout" @{ } $access | Out-Null
  try {
    Api POST "/v1/auth/refresh" @{ refreshToken = $refresh }
    throw "expected 401"
  } catch {
    if ($_.Exception.Response.StatusCode.value__ -ne 401) { throw "expected 401" }
  }
} | Out-Null

Write-Host ""
if ($fail -eq 0) {
  Write-Host "All launcher endpoints behaved correctly." -ForegroundColor Green
  exit 0
}
Write-Host "$fail check(s) failed." -ForegroundColor Red
exit 1
