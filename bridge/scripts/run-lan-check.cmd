@echo off
setlocal
cd /d "%~dp0.."
call bun run dev -- --lan-check %*
