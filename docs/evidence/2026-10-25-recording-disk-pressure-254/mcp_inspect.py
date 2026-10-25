import asyncio,json,os,sys
from mcp import ClientSession,StdioServerParameters
from mcp.client.stdio import stdio_client
async def main():
    params=StdioServerParameters(command=sys.argv[1],args=['--cli',sys.argv[2],'--socket',sys.argv[3]],env=dict(os.environ))
    async with stdio_client(params) as (read,write):
        async with ClientSession(read,write) as client:
            await client.initialize()
            result=await client.call_tool('get_recording',{'id':sys.argv[4]})
            assert not result.isError
            print(json.dumps(result.structuredContent))
asyncio.run(main())
