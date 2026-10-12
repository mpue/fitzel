using System.Collections.Generic;
using System.Text.Json;
using static Fitzel.LuaApi;

namespace Fitzel
{
    /// <summary>
    /// Lockstep multiplayer through fitzel's relay (Lua's <c>net.*</c>). Everything is
    /// polled: <see cref="Poll"/> once a frame is the whole network loop. The protocol
    /// (HELLO, CREATE, JOIN, START, CMD, TURN ...) is the one in lua-scripting.md.
    /// Play ending closes every connection and a hosted relay.
    /// </summary>
    public static class Net
    {
        /// <summary>Connect to a relay (done once Status is "connected"). False + why.</summary>
        public static bool Connect(string host, int port, out string error)
        {
            var r = Call("net", "connect", host, port);
            error = Bool(r, 0) ? null : Str(r, 1, "");
            return error == null;
        }

        public static bool Connect(string host, int port = 27960) => Connect(host, port, out _);

        /// <summary>"idle", "connecting", "connected" or "closed", and the last error.</summary>
        public static (string state, string error) Status
        {
            get
            {
                var r = Call("net", "status");
                return (Str(r, 0, "idle"), Str(r, 1, ""));
            }
        }

        public static bool Send(string message) => Bool(Call("net", "send", message), 0);

        /// <summary>What arrived since the last call; also drives connecting and a hosted relay.</summary>
        public static List<string> Poll()
        {
            var l = new List<string>();
            foreach (var m in Items(Call("net", "poll")[0])) l.Add(Str(m, ""));
            return l;
        }

        public static void Close() => Call("net", "close");

        /// <summary>Host the relay in this game. The port it listens on, or null + why.</summary>
        public static int? Serve(int port, int turnMs, out string error)
        {
            var r = Call("net", "serve", port, turnMs);
            error = Bool(r, 0) ? null : Str(r, 1, "");
            return error == null ? Int(r, 1) : null;
        }

        public static int? Serve(int port = 27960, int turnMs = 100) => Serve(port, turnMs, out _);

        public static void StopServer() => Call("net", "stopServer");

        /// <summary>The hosted relay: running, port, clients, rooms.</summary>
        public static (bool running, int port, int clients, int rooms) ServerInfo
        {
            get
            {
                var r = Call("net", "serverInfo");
                return (Bool(r, 0), Int(r, 1), Int(r, 2), Int(r, 3));
            }
        }

        /// <summary>This machine's IPv4 addresses (what a host tells the others).</summary>
        public static List<string> Addresses()
        {
            var l = new List<string>();
            foreach (var a in Items(Call("net", "addresses")[0])) l.Add(Str(a, ""));
            return l;
        }

        /// <summary>A monotonic clock in seconds.</summary>
        public static double Clock => Num(Call("net", "clock"), 0);

        /// <summary>In the browser the host the page came from; null on the desktop.</summary>
        public static string PageHost => Str(Call("net", "pageHost"), 0);
    }

    /// <summary>One answer of the language model (llm.poll).</summary>
    public class LlmAnswer
    {
        public int Id;
        public bool Ok;
        /// <summary>The reply without its &lt;think&gt; part.</summary>
        public string Text, Error;
        /// <summary>The reply as data, when the request set a format and valid JSON came.</summary>
        public JsonElement Data;
        public float Ms;
        public int Tokens;
    }

    /// <summary>
    /// A language model on this machine through Ollama's HTTP API (Lua's <c>llm.*</c>).
    /// Nothing blocks: <see cref="Chat(object)"/> queues a request and returns its number at
    /// once, <see cref="Poll"/> hands over what came back since.
    /// </summary>
    public static class Llm
    {
        /// <summary>
        /// Queue a request in the shape of Ollama's /api/chat: messages, or prompt (one user
        /// message), system, model, format ("json" or a schema), options, priority ... --
        /// a dictionary or an anonymous object: <c>Llm.Chat(new { prompt = "Hello", options = new { temperature = 0.8 } })</c>.
        /// </summary>
        public static int Chat(object request) => Int(Call("llm", "chat", request), 0);

        /// <summary>A short request: one prompt, optionally a system line.</summary>
        public static int Chat(string prompt, string system = null) =>
            Chat(system == null ? (object)new { prompt } : new { prompt, system });

        public static List<LlmAnswer> Poll()
        {
            var l = new List<LlmAnswer>();
            foreach (var a in Items(Call("llm", "poll")[0]))
                l.Add(new LlmAnswer
                {
                    Id = IntField(a, "id"), Ok = BoolField(a, "ok"), Text = StrField(a, "text"),
                    Error = StrField(a, "error"), Data = Field(a, "data"), Ms = NumField(a, "ms"),
                    Tokens = IntField(a, "tokens"),
                });
            return l;
        }

        /// <summary>Requests waiting plus the one running.</summary>
        public static int Pending => Int(Call("llm", "pending"), 0);
        /// <summary>Drop the waiting requests (not the running one).</summary>
        public static void Clear() => Call("llm", "clear");
        public static void SetHost(string host, int? port = null)
        {
            if (port.HasValue) Call("llm", "setHost", host, port.Value);
            else Call("llm", "setHost", host);
        }
        /// <summary>The default model for requests without one.</summary>
        public static string Model
        {
            get => Str(Call("llm", "model"), 0, "");
            set => Call("llm", "setModel", value);
        }
    }
}
