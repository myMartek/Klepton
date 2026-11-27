// Bigscreen Beta: films from places of the player's choosing ("Locations").
//
// Bigscreen lists its local films from /storage/emulated/0/Movies, which is
// Documents/android-files/Movies in this app's container; until 2026-11-24 the
// only way in was copying with devicectl from a Mac. A location is one of two
// kinds, and each appears in the game as a folder inside Movies:
//
// - A folder of the Files app, picked in the system's folder picker ("On My
//   Apple Vision Pro", iCloud Drive, or any folder in them). It is kept as a
//   bookmark; at every start its access is opened for the whole run, an empty
//   folder of the same name is made inside Movies so the game's listing shows
//   it, and the runtime serves everything at and below that name from the
//   picked folder (kl_guest_dir_aliases_set, runtime/libc/kl_libc.c).
// - A pCloud public link to a folder (no account, as Explore360 reads shared
//   lists; KleptonPCloud.swift). Its tree is mirrored as placeholder files in
//   Application Support/KleptonPCloud/<code>: folders as folders, every film
//   as a sparse file as long as the film (no bytes on disk), served as
//   Movies/<name> the same way. The runtime is told which placeholder is which
//   file on pCloud (kl_remote_files_set, runtime/media/kl_remote.m), and the
//   player reads a film from there by range while it plays: nothing is copied,
//   only a few megabytes are kept in memory. The tree is the last one read, so
//   the folder is there at once; it is read again at every start.
//
// Nothing is copied, and the game's own Movies folder stays as it was.
// Bigscreen only: no other game offers this or registers anything, so their
// paths stay exactly as they were.
import Foundation
import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class KLBigscreenFolders: ObservableObject {
    static let shared = KLBigscreenFolders()
    /// The one game that lists films from Movies.
    static var offered: Bool { klTargetName() == "x2497738113633933" }

    struct Folder: Codable, Identifiable, Equatable {
        var name: String        // the folder's name inside Movies
        var bookmark: Data
        var id: String { name }
    }

    struct PCloudLink: Codable, Identifiable, Equatable {
        var name: String        // the folder's name inside Movies
        var code: String
        var host: String?       // the data centre that answered last
        var link: String        // as pasted, to show
        var id: String { name }
    }

    /// One film of a pCloud tree: where its placeholder is, below the tree's folder.
    struct RemoteFile: Codable, Equatable {
        var path: String
        var fileid: Int64
        var size: Int64
    }

    // The Files folders keep the key they had when they were on the game page
    // (2026-11-24), so every folder picked then is still there.
    private static let key = "bigscreen.filesFolders"
    private static let pcloudKey = "bigscreen.pcloudLinks"
    @Published private(set) var folders: [Folder] = []
    @Published private(set) var pclouds: [PCloudLink] = []
    /// Locations that cannot be reached now (a deleted folder, iCloud signed out,
    /// a pCloud link that is gone), with what to say about it.
    @Published private(set) var unreachable: [String: String] = [:]
    /// pCloud links being read now.
    @Published private(set) var reading: Set<String> = []
    /// Films found in each pCloud tree, to show.
    @Published private(set) var filmCount: [String: Int] = [:]
    /// Picked URLs whose access is open; each is closed when its folder is removed.
    private var open: [String: URL] = [:]
    private var trees: [String: [RemoteFile]] = [:]

    private init() {
        if let d = UserDefaults.standard.data(forKey: Self.key),
           let f = try? JSONDecoder().decode([Folder].self, from: d) {
            folders = f
        }
        if let d = UserDefaults.standard.data(forKey: Self.pcloudKey),
           let p = try? JSONDecoder().decode([PCloudLink].self, from: d) {
            pclouds = p
        }
        for p in pclouds {
            trees[p.name] = Self.loadTree(p)
            filmCount[p.name] = trees[p.name]?.count ?? 0
        }
    }

    /// How many places beside the game's own folder, for the start window.
    var count: Int { folders.count + pclouds.count }
    var names: [String] { folders.map(\.name) + pclouds.map(\.name) }

    private static var movies: URL {
        URL(fileURLWithPath: Paths.container).appendingPathComponent("android-files/Movies")
    }

    private func save() {
        if let d = try? JSONEncoder().encode(folders) { UserDefaults.standard.set(d, forKey: Self.key) }
        if let d = try? JSONEncoder().encode(pclouds) { UserDefaults.standard.set(d, forKey: Self.pcloudKey) }
    }

    /// The two places the Files app shows at the top answer their on-disk names
    /// to localizedName: "On My Apple Vision Pro" came back as "File Provider
    /// Storage" (seen on the headset, 2026-11-24). Those two get the names the
    /// Files app shows; any other folder keeps its own.
    private static func rootName(_ raw: String) -> String? {
        switch raw {
        case "File Provider Storage": return klT("On My Apple Vision Pro", "Auf meiner Apple Vision Pro")
        case "com~apple~CloudDocs": return "iCloud Drive"
        default: return nil
        }
    }

    /// A name made safe for a single path component and distinct from the other
    /// locations and from what the game's own Movies folder already holds.
    private func freeName(_ shown: String) -> String {
        var base = Self.safe(shown)
        if base.isEmpty { base = klT("Folder", "Ordner") }
        let taken = Set(names)
        let fm = FileManager.default
        func free(_ n: String) -> Bool {
            !taken.contains(n) && !fm.fileExists(atPath: Self.movies.appendingPathComponent(n).path)
        }
        if free(base) { return base }
        var i = 2
        while !free("\(base) \(i)") { i += 1 }
        return "\(base) \(i)"
    }

    private static func safe(_ s: String) -> String {
        var t = s.replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: ":", with: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        while t.hasPrefix(".") { t.removeFirst() }
        return t
    }

    // MARK: Files folders

    /// A folder chosen in the picker. Returns a message for the player on failure.
    func add(_ url: URL) -> String? {
        let scoped = url.startAccessingSecurityScopedResource()
        guard let data = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            if scoped { url.stopAccessingSecurityScopedResource() }
            return klT("This folder cannot be kept. Choose another one.",
                       "Dieser Ordner lässt sich nicht merken. Wähle einen anderen.")
        }
        if let same = folders.first(where: { open[$0.name]?.standardizedFileURL == url.standardizedFileURL }) {
            if scoped { url.stopAccessingSecurityScopedResource() }
            return klT("\"\(same.name)\" is already there.", "„\(same.name)“ ist schon da.")
        }
        let shown = Self.rootName(url.lastPathComponent)
            ?? (try? url.resourceValues(forKeys: [.localizedNameKey]).localizedName) ?? url.lastPathComponent
        let f = Folder(name: freeName(shown), bookmark: data)
        folders.append(f)
        open[f.name] = url
        save()
        NSLog("[bigscreen] Files folder added: \"\(f.name)\" -> \(url.path)")
        register()
        return nil
    }

    func remove(_ f: Folder) {
        folders.removeAll { $0.name == f.name }
        save()
        register()
        open.removeValue(forKey: f.name)?.stopAccessingSecurityScopedResource()
        unreachable.removeValue(forKey: f.name)
        // Only the empty stand-in folder; rmdir refuses anything that holds files.
        rmdir(Self.movies.appendingPathComponent(f.name).path)
        NSLog("[bigscreen] Files folder removed: \"\(f.name)\"")
    }

    // MARK: pCloud links

    /// The folder that holds a link's placeholders, and the file that remembers its tree.
    private static var pcloudRoot: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("KleptonPCloud")
    }
    private static func treeDir(_ p: PCloudLink) -> URL { pcloudRoot.appendingPathComponent(p.code) }
    private static func treeFile(_ p: PCloudLink) -> URL { pcloudRoot.appendingPathComponent(p.code + ".json") }

    private static func loadTree(_ p: PCloudLink) -> [RemoteFile] {
        guard let d = try? Data(contentsOf: treeFile(p)),
              let t = try? JSONDecoder().decode([RemoteFile].self, from: d) else { return [] }
        return t
    }

    /// The containers Bigscreen's own browser lists (its dex, 2026-11-25).
    private static let filmTypes: Set<String> = ["mp4", "m4v", "mov", "mkv", "avi", "webm", "m2ts", "ts",
                                                 "mpg", "mpeg", "ogv", "wmv"]

    /// A pasted link: read it, mirror it, show it. Returns a message on failure.
    func addPCloud(_ text: String) async -> String? {
        guard let link = KLPCloud.Link(text) else { return KLPCloudError.notALink.localizedDescription }
        if let same = pclouds.first(where: { $0.code == link.code }) {
            return klT("\"\(same.name)\" is already there.", "„\(same.name)“ ist schon da.")
        }
        let pending = "pcloud:" + link.code
        reading.insert(pending)
        defer { reading.remove(pending) }
        do {
            let (item, host) = try await KLPCloud.show(link)
            let shown = item.isfolder ? item.name : (item.name as NSString).deletingPathExtension
            let p = PCloudLink(name: freeName(shown.isEmpty ? "pCloud" : shown), code: link.code, host: host,
                               link: text.trimmingCharacters(in: .whitespacesAndNewlines))
            let files = try Self.mirror(item, into: Self.treeDir(p))
            try? JSONEncoder().encode(files).write(to: Self.treeFile(p))
            pclouds.append(p)
            trees[p.name] = files
            filmCount[p.name] = files.count
            save()
            register()
            NSLog("[bigscreen] pCloud link added: \"\(p.name)\", \(files.count) film(s) (\(host))")
            return nil
        } catch {
            NSLog("[bigscreen] pCloud link refused: \(error.localizedDescription)")
            return klT("pCloud could not be read: ", "pCloud ließ sich nicht lesen: ") + error.localizedDescription
        }
    }

    func remove(_ p: PCloudLink) {
        pclouds.removeAll { $0.code == p.code }
        trees.removeValue(forKey: p.name)
        filmCount.removeValue(forKey: p.name)
        unreachable.removeValue(forKey: p.name)
        save()
        register()
        // Our own placeholders only: the tree folder this app made, and its list.
        try? FileManager.default.removeItem(at: Self.treeDir(p))
        try? FileManager.default.removeItem(at: Self.treeFile(p))
        rmdir(Self.movies.appendingPathComponent(p.name).path)
        NSLog("[bigscreen] pCloud link removed: \"\(p.name)\"")
    }

    /// Read every link again and bring its placeholders in line (at every start).
    private func refreshPClouds() {
        for p in pclouds {
            reading.insert(p.name)
            Task {
                defer { reading.remove(p.name) }
                do {
                    let (item, host) = try await KLPCloud.show(KLPCloud.Link(code: p.code, preferring: p.host))
                    let files = try Self.mirror(item, into: Self.treeDir(p))
                    try? JSONEncoder().encode(files).write(to: Self.treeFile(p))
                    if let i = pclouds.firstIndex(where: { $0.code == p.code }), pclouds[i].host != host {
                        pclouds[i].host = host
                        save()
                    }
                    unreachable.removeValue(forKey: p.name)
                    if files != trees[p.name] {
                        trees[p.name] = files
                        filmCount[p.name] = files.count
                        register()
                    }
                    NSLog("[bigscreen] pCloud \"\(p.name)\": \(files.count) film(s)")
                } catch {
                    // The tree read last time stays; its films play while their links answer.
                    unreachable[p.name] = error.localizedDescription
                    NSLog("[bigscreen] pCloud \"\(p.name)\" not read: \(error.localizedDescription)")
                }
            }
        }
    }

    /// The tree as folders and placeholder films below `dir`; whatever was
    /// there and is no longer in the tree goes. Returns the films.
    private static func mirror(_ root: KLPCloudItem, into dir: URL) throws -> [RemoteFile] {
        let fm = FileManager.default
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        var d = dir
        var rv = URLResourceValues()
        rv.isExcludedFromBackup = true
        try? d.setResourceValues(rv)
        var files: [RemoteFile] = []
        var dirs: Set<String> = []
        func walk(_ item: KLPCloudItem, _ rel: String) {
            for c in item.contents ?? [] {
                let n = safe(c.name)
                guard !n.isEmpty else { continue }
                let path = rel.isEmpty ? n : rel + "/" + n
                if c.isfolder {
                    dirs.insert(path)
                    walk(c, path)
                } else if let id = c.fileid, let size = c.size, size > 0,
                          filmTypes.contains((n as NSString).pathExtension.lowercased()) {
                    files.append(RemoteFile(path: path, fileid: id, size: size))
                }
            }
        }
        if root.isfolder {
            walk(root, "")
        } else if let id = root.fileid, let size = root.size, size > 0 {
            files.append(RemoteFile(path: safe(root.name), fileid: id, size: size))
        }
        let wanted = Set(files.map(\.path))
        // What is there and no longer wanted (deepest first, so folders empty out).
        if let en = fm.enumerator(at: dir, includingPropertiesForKeys: [.isDirectoryKey]) {
            var stale: [URL] = []
            let base = dir.standardizedFileURL.path + "/"
            while let u = en.nextObject() as? URL {
                let rel = String(u.standardizedFileURL.path.dropFirst(base.count))
                let isDir = (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
                if isDir ? !dirs.contains(rel) : !wanted.contains(rel) { stale.append(u) }
            }
            for u in stale.reversed() { try? fm.removeItem(at: u) }
        }
        for p in dirs.sorted() {
            try? fm.createDirectory(at: dir.appendingPathComponent(p), withIntermediateDirectories: true)
        }
        for f in files {
            let u = dir.appendingPathComponent(f.path)
            let now = (try? fm.attributesOfItem(atPath: u.path)[.size] as? NSNumber)?.int64Value
            if now == f.size { continue }
            // Sparse: as long as the film, nothing on disk.
            fm.createFile(atPath: u.path, contents: nil)
            truncate(u.path, off_t(f.size))
        }
        return files
    }

    // MARK: Start

    /// Before the guest boots: open every kept folder and tell the runtime.
    func applyAtBoot() {
        guard Self.offered else { return }
        // Movies itself, always. Bigscreen's video player opens there only when
        // the folder exists; without it the player opened in the game's own APK
        // ("No supported videos") and Back led to the container's Documents
        // (seen on the headset, 2026-11-24). The game never asks Java for the
        // folder, so nothing else creates it.
        try? FileManager.default.createDirectory(at: Self.movies, withIntermediateDirectories: true)
        var changed = false
        // A root kept under its on-disk name by the first test build gets the
        // Files app's name; its empty stand-in folder goes with the old name.
        for (i, f) in folders.enumerated() where open[f.name] == nil {
            guard let shown = Self.rootName(f.name), !folders.contains(where: { $0.name == shown }) else { continue }
            rmdir(Self.movies.appendingPathComponent(f.name).path)
            folders[i].name = shown
            changed = true
        }
        for (i, f) in folders.enumerated() where open[f.name] == nil {
            var stale = false
            guard let u = try? URL(resolvingBookmarkData: f.bookmark, options: [], relativeTo: nil,
                                   bookmarkDataIsStale: &stale) else {
                unreachable[f.name] = klT("Cannot be reached. Remove it and choose it again.",
                                          "Nicht erreichbar. Entfernen und neu wählen.")
                NSLog("[bigscreen] Files folder \"\(f.name)\" cannot be reached")
                continue
            }
            _ = u.startAccessingSecurityScopedResource()
            open[f.name] = u
            unreachable.removeValue(forKey: f.name)
            if stale, let d = try? u.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
                folders[i].bookmark = d
                changed = true
            }
        }
        if changed { save() }
        register()
        refreshPClouds()
    }

    /// The runtime's lists, and the stand-in folders the game's listing shows.
    private func register() {
        let fm = FileManager.default
        var lines: [String] = []
        for f in folders {
            guard let u = open[f.name] else { continue }
            try? fm.createDirectory(at: Self.movies.appendingPathComponent(f.name), withIntermediateDirectories: true)
            // An iCloud folder that is not on the headset yet lists nothing until it
            // is asked for (LauncherFiles.prepare found the same). Only the folders
            // are asked for here; a film downloads when the game reads it.
            kickFolders(u)
            lines.append("/Movies/\(f.name)\t\(u.path)")
        }
        var films: [String] = []
        for p in pclouds {
            let dir = Self.treeDir(p)
            guard fm.fileExists(atPath: dir.path) else { continue }
            try? fm.createDirectory(at: Self.movies.appendingPathComponent(p.name), withIntermediateDirectories: true)
            lines.append("/Movies/\(p.name)\t\(dir.path)")
            for f in trees[p.name] ?? [] {
                films.append([dir.appendingPathComponent(f.path).path, "pcloud", p.host ?? KLPCloud.euHost,
                              p.code, String(f.fileid), String(f.size)].joined(separator: "\t"))
            }
        }
        kl_remote_files_set(films.isEmpty ? nil : films.joined(separator: "\n"))
        kl_guest_dir_aliases_set(lines.isEmpty ? nil : lines.joined(separator: "\n"))
    }

    private nonisolated func kickFolders(_ root: URL) {
        Task.detached(priority: .utility) {
            let fm = FileManager.default
            guard (try? root.resourceValues(forKeys: [.isUbiquitousItemKey]).isUbiquitousItem) == true else { return }
            try? fm.startDownloadingUbiquitousItem(at: root)
            guard let en = fm.enumerator(at: root, includingPropertiesForKeys: [.isDirectoryKey],
                                         options: [.skipsHiddenFiles]) else { return }
            var n = 0
            while let u = en.nextObject() as? URL, n < 200 {
                if (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                    try? fm.startDownloadingUbiquitousItem(at: u)
                    n += 1
                }
            }
        }
    }
}

/// "4 custom locations", for the start window and the settings sidebar.
func klLocationsCount(_ n: Int) -> String {
    switch n {
    case 0: return klT("No custom locations", "Keine eigenen Orte")
    case 1: return klT("1 custom location", "1 eigener Ort")
    default: return klT("\(n) custom locations", "\(n) eigene Orte")
    }
}

/// Under the start window's buttons (Bigscreen only): how many places beside
/// Movies the game lists, and the way to the page that manages them.
struct BigscreenLocationsLine: View {
    @ObservedObject private var store = KLBigscreenFolders.shared
    let manage: () -> Void

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 14) {
                Label(klLocationsCount(store.count), systemImage: "folder")
                    .font(.callout)
                Button(klT("Manage Locations", "Orte verwalten"), systemImage: "folder.badge.gearshape") { manage() }
                    .controlSize(.small)
            }
            if !store.names.isEmpty {
                Text(store.names.joined(separator: " · "))
                    .font(.caption).foregroundStyle(.secondary)
                    .lineLimit(1)
                    .frame(maxWidth: 560)
            }
        }
    }
}

/// The Locations page of the settings window (KLSettingsPageID.locations).
struct BigscreenLocationsView: View {
    @ObservedObject private var store = KLBigscreenFolders.shared
    @State private var picking = false
    @State private var message = ""
    @State private var linkText = ""
    @State private var linkMessage = ""

    var body: some View {
        KLSection(title: klT("Bigscreen's own folder", "Eigener Ordner des Spiels")) {
            HStack {
                Image(systemName: "film")
                Text("Movies").bold()
                Spacer()
                Text(klT("always there", "immer da")).font(.caption).foregroundStyle(.secondary)
            }
        }
        KLSection(title: klT("Folders of the Files app", "Ordner der Dateien-App")) {
            ForEach(store.folders) { f in
                row(f.name, symbol: "folder", note: store.unreachable[f.name]) { store.remove(f) }
            }
            Button { message = ""; picking = true } label: {
                Label(klT("Add a folder…", "Ordner hinzufügen …"), systemImage: "folder.badge.plus")
            }
            if !message.isEmpty { Text(message).font(.footnote) }
        }
        .klHelp("bigscreen.folders")
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { r in
            switch r {
            case .success(let u): message = store.add(u) ?? klT("Added. Bigscreen shows it the next time it starts.",
                                                                  "Hinzugefügt. Bigscreen zeigt ihn beim nächsten Start.")
            case .failure(let e): message = klT("No folder chosen: \(e.localizedDescription)",
                                                 "Kein Ordner gewählt: \(e.localizedDescription)")
            }
        }
        KLSection(title: klT("pCloud links", "pCloud-Links")) {
            ForEach(store.pclouds) { p in
                let films = store.filmCount[p.name] ?? 0
                let count = films == 1 ? klT("1 film", "1 Film") : klT("\(films) films", "\(films) Filme")
                row(p.name, symbol: "cloud", detail: store.reading.contains(p.name) ? klT("Reading…", "Wird gelesen …") : count,
                    note: store.unreachable[p.name]) { store.remove(p) }
            }
            HStack {
                TextField("https://e.pcloud.link/publink/show?code=…", text: $linkText)
                    .textFieldStyle(.roundedBorder)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .onSubmit(addLink)
                if store.reading.contains(where: { $0.hasPrefix("pcloud:") }) {
                    ProgressView()
                } else {
                    Button(klT("Add", "Hinzufügen"), systemImage: "plus.circle", action: addLink)
                        .disabled(linkText.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            if !linkMessage.isEmpty { Text(linkMessage).font(.footnote) }
        }
        .klHelp("bigscreen.pcloud")
        Text(klT("Bigscreen shows each location as a folder inside Movies. Removing one here deletes nothing in the Files app or on pCloud.",
                 "Bigscreen zeigt jeden Ort als Ordner in Movies. Hier entfernen löscht nichts in der Dateien-App oder bei pCloud."))
            .font(.caption).foregroundStyle(.secondary)
    }

    private func addLink() {
        let text = linkText
        guard !text.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        linkMessage = ""
        Task {
            if let err = await store.addPCloud(text) {
                linkMessage = err
            } else {
                linkText = ""
                linkMessage = klT("Added. Bigscreen shows it the next time it starts.",
                                  "Hinzugefügt. Bigscreen zeigt ihn beim nächsten Start.")
            }
        }
    }

    private func row(_ name: String, symbol: String, detail: String? = nil, note: String?,
                     remove: @escaping () -> Void) -> some View {
        HStack {
            Image(systemName: note != nil ? "exclamationmark.triangle" : symbol)
            VStack(alignment: .leading, spacing: 2) {
                Text(name)
                if let note { Text(note).font(.caption).foregroundStyle(.secondary) }
            }
            Spacer()
            if let detail { Text(detail).font(.caption).foregroundStyle(.secondary) }
            Button(role: .destructive, action: remove) {
                Label(klT("Remove", "Entfernen"), systemImage: "minus.circle")
            }
            .labelStyle(.iconOnly)
        }
    }
}
