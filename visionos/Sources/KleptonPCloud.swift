// pCloud public links, without an account: what Bigscreen Beta's pCloud
// locations read (KleptonBigscreenFolders.swift).
//
// The same two calls Explore360 makes for a shared list (its
// Cloud/PCloudClient.swift, "Public links"): showpublink gives the tree a
// link points to, recursively, and getpublinkdownload a short-lived direct
// link to one file in it. The second is made by the runtime when a film is
// played (runtime/media/kl_remote.m), so here only the listing is asked for.
// A link made in the EU data centre answers only on eapi.pcloud.com, one made
// in the US only on api.pcloud.com; the host that answered is kept and tried
// first the next time.
import Foundation

struct KLPCloudError: LocalizedError, Sendable {
    let code: Int
    let message: String
    var errorDescription: String? {
        switch code {
        case 7001, 7002:
            return klT("pCloud does not know this link (any more).", "pCloud kennt diesen Link nicht (mehr).")
        case -2:
            return klT("This is not a pCloud link.", "Das ist kein pCloud-Link.")
        default:
            return "pCloud: \(message) (\(code))"
        }
    }
    static let badResponse = KLPCloudError(code: -1, message: "unexpected answer")
    static let notALink = KLPCloudError(code: -2, message: "not a link")
}

/// One file or folder of a public link's tree.
struct KLPCloudItem: Decodable, Sendable {
    let name: String
    let isfolder: Bool
    let fileid: Int64?
    let size: Int64?
    let contents: [KLPCloudItem]?
}

enum KLPCloud {
    static let usHost = "api.pcloud.com"
    static let euHost = "eapi.pcloud.com"

    /// A pasted link reduced to its code and the hosts to ask, in order.
    struct Link: Sendable, Equatable {
        let code: String
        let hosts: [String]

        init(code: String, preferring host: String?) {
            self.code = code
            let other = host == KLPCloud.euHost ? KLPCloud.usHost : KLPCloud.euHost
            hosts = host.map { [$0, other] } ?? [KLPCloud.euHost, KLPCloud.usHost]
        }

        /// Every form pCloud hands out: `https://u.pcloud.link/publink/show?code=…`,
        /// `https://e.pcloud.link/publink/show?code=…`, `https://my.pcloud.com/#page=publink&code=…`,
        /// `https://pc.cd/<code>`, or the bare code.
        init?(_ raw: String) {
            let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !text.isEmpty else { return nil }
            let lower = text.lowercased()
            let euFirst = lower.contains("e.pcloud.link") || lower.contains("eapi.")
            let usFirst = lower.contains("u.pcloud.link")
            hosts = usFirst && !euFirst ? [KLPCloud.usHost, KLPCloud.euHost] : [KLPCloud.euHost, KLPCloud.usHost]
            if let r = text.range(of: #"code=([A-Za-z0-9]+)"#, options: .regularExpression) {
                code = String(text[r].dropFirst("code=".count))
            } else if let url = URL(string: text), url.host?.lowercased() == "pc.cd", url.pathComponents.count > 1 {
                code = url.pathComponents[1]
            } else if text.range(of: #"^[A-Za-z0-9]{6,}$"#, options: .regularExpression) != nil {
                code = text
            } else {
                return nil
            }
        }
    }

    /// The tree a link points to (recursive for a folder) and the host that answered.
    static func show(_ link: Link) async throws -> (item: KLPCloudItem, host: String) {
        struct R: Decodable { let metadata: KLPCloudItem }
        var last: Error = KLPCloudError.badResponse
        for host in link.hosts {
            do {
                let data = try await call(host, "showpublink", ["code": link.code])
                return (try decode(data, as: R.self).metadata, host)
            } catch let e as KLPCloudError where e.code == 7001 || e.code == 7002 {
                last = e        // unknown in this data centre: the other one
            }
        }
        throw last
    }

    private static func call(_ host: String, _ method: String, _ params: [String: String]) async throws -> Data {
        var req = URLRequest(url: URL(string: "https://\(host)/\(method)")!)
        req.httpMethod = "POST"
        req.timeoutInterval = 30
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        req.httpBody = Data(params.map { k, v in
            "\(k)=\(v.addingPercentEncoding(withAllowedCharacters: allowed) ?? v)"
        }.joined(separator: "&").utf8)
        return try await URLSession.shared.data(for: req).0
    }

    private struct Envelope: Decodable {
        let result: Int
        let error: String?
    }

    private static func decode<T: Decodable>(_ data: Data, as: T.Type) throws -> T {
        let env = try JSONDecoder().decode(Envelope.self, from: data)
        guard env.result == 0 else { throw KLPCloudError(code: env.result, message: env.error ?? "error") }
        return try JSONDecoder().decode(T.self, from: data)
    }
}
