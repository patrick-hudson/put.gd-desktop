"""Shared official-SDK session for lab drivers; imports stay lazy for host tests."""
from contextlib import asynccontextmanager


@asynccontextmanager
async def helper_session(command, arguments):
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    parameters=StdioServerParameters(command=command,args=arguments)
    async with stdio_client(parameters) as (read,write):
        async with ClientSession(read,write) as session:
            await session.initialize()
            yield session
