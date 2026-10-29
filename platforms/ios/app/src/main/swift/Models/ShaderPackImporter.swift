// ShaderPackImporter.swift — installs a picked .zip or folder into Documents/shaders
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum ShaderPackImportError: LocalizedError {
    case noUserRoot
    case notAShaderPack

    var errorDescription: String? {
        switch self {
        case .noUserRoot:
            return "Your shader folder couldn't be opened."
        case .notAShaderPack:
            return "No shader presets were found in it, so nothing was installed."
        }
    }
}

/// A pack offered by name beside the base pack. It comes from its repository's latest GitHub
/// release, so a new upstream version reaches users without an app update.
struct ShaderPackSource: Identifiable, Sendable {
    /// Also its folder under Documents/shaders, so a second download replaces the first.
    let name: String
    let repository: String
    /// Matched against a whole asset name: a release can ship several zips, versioned by name.
    let asset: String
    /// The archive folder that becomes the pack, when the rest of the archive is not shaders.
    var subtree: String? = nil
    /// Its presets name stages in RetroArch Slang Shaders by relative path.
    var needsBasePack = false

    var id: String { name }
}

/// Installs a shader pack into the user root under a free folder name.
@MainActor
final class ShaderPackImporter: ObservableObject {
    @Published private(set) var installing: Set<String> = []
    @Published private(set) var errors: [String: String] = [:]
    @Published private(set) var installedName: String?
    @Published private(set) var installProblem: ShaderPresetFailure?

    var isBusy: Bool { !installing.isEmpty }

    /// `named` replaces the archive's own name, for a staging file named by UUID. Callers use the
    /// returned folder name, since concurrent installs overwrite `installedName`.
    @discardableResult
    func install(archiveAt source: URL, named: String? = nil) async -> String? {
        await install(source, named: named, writing: Self.extract)
    }

    @discardableResult
    func install(folderAt source: URL) async -> String? {
        await install(source, named: nil, writing: Self.copyTree)
    }

    static let basePackURL = URL(string: "https://buildbot.libretro.com/assets/frontend/shaders_slang.zip")!
    static let basePackBytes: Int64 = 54_000_000

    nonisolated static let packs: [ShaderPackSource] = [
        // The release zip is the whole repo; the RetroArch chain sits beside a ReShade port.
        ShaderPackSource(
            name: "CRT-SatPixie", repository: "Conkwer/satpixie-crt-shader",
            asset: #"satpixie-crt-shader-\d+\.zip"#,
            subtree: "RetroArch/shaders/shaders_slang/crt"),
        // Its release also ships 86Box, Amiberry and Kodi ports, so the pattern pins RetroArch's.
        ShaderPackSource(
            name: "Lola Retro", repository: "zoran123456/lola-retro",
            asset: #"lola-retro-v[\d.]+\.zip"#),
        ShaderPackSource(
            name: "Retro Crisis GDV-NTSC", repository: "RetroCrisis/Retro-Crisis-GDV-NTSC",
            asset: #"Retro\.Crisis\.GDV-NTSC\..+\.zip"#,
            needsBasePack: true),
    ]

    func installBasePack() async {
        await download(ShaderPresetLibrary.basePackFolderName, from: { Self.basePackURL }, writing: Self.extract)
    }

    func installPack(_ pack: ShaderPackSource) async {
        await download(pack.name, from: { try await Self.latestAsset(of: pack) }) { source, destination in
            try Self.extract(source, destination)
            try Self.place(pack, in: destination)
        }
    }

    /// Lands in the folder named `key`, replacing an earlier copy instead of sitting beside it.
    private func download(
        _ key: String,
        from link: () async throws -> URL,
        writing: @escaping @Sendable (URL, URL) throws -> Void
    ) async {
        installing.insert(key)
        errors[key] = nil
        defer { installing.remove(key) }
        do {
            let staged = try await ShaderCatalogInstaller.stage(try await link())
            defer { try? FileManager.default.removeItem(at: staged) }
            guard let landed = await install(staged, named: key, writing: writing) else {
                errors[key] = errors.removeValue(forKey: staged.lastPathComponent)
                    ?? ShaderPackImportError.notAShaderPack.localizedDescription
                return
            }
            installedName = nil
            try await Task.detached(priority: .userInitiated) { try Self.replace(key, with: landed) }.value
            installedName = key
        } catch {
            errors[key] = error.localizedDescription
        }
    }

    private nonisolated static func replace(_ name: String, with landed: String) throws {
        guard landed != name, let root = ShaderPresetLibrary.userRoot else { return }
        let base = root.appendingPathComponent(name, isDirectory: true)
        try FileManager.default.removeItem(at: base)
        try FileManager.default.moveItem(at: root.appendingPathComponent(landed, isDirectory: true), to: base)
    }

    private struct Release: Decodable {
        struct Asset: Decodable {
            let name: String
            let url: URL

            enum CodingKeys: String, CodingKey {
                case name
                case url = "browser_download_url"
            }
        }

        let assets: [Asset]
    }

    private static func latestAsset(of pack: ShaderPackSource) async throws -> URL {
        guard let api = URL(string: "https://api.github.com/repos/\(pack.repository)/releases/latest"),
              let pattern = try? Regex(pack.asset) else {
            throw ShaderCatalogInstallError.unusableLink
        }
        var request = URLRequest(url: api)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await URLSession.shared.data(for: request)
        } catch {
            throw ShaderCatalogInstallError.unreachable
        }
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw ShaderCatalogInstallError.serverRefused(http.statusCode)
        }
        guard let release = try? JSONDecoder().decode(Release.self, from: data),
              let asset = release.assets.first(where: { $0.name.wholeMatch(of: pattern) != nil }) else {
            throw ShaderCatalogInstallError.unusableLink
        }
        return asset.url
    }

    /// Keeps only the folder of the archive that is the pack, moved up to the pack folder.
    private nonisolated static func place(_ pack: ShaderPackSource, in destination: URL) throws {
        var kept = pack.subtree ?? ""
        if pack.needsBasePack {
            guard let wrapping = companionWrapping(in: destination) else {
                throw ShaderPackImportError.notAShaderPack
            }
            kept = wrapping
        }
        guard !kept.isEmpty else { return }
        let inner = destination.appendingPathComponent(kept, isDirectory: true)
        // An upstream release that moved its files would otherwise fail with a raw move error.
        guard FileManager.default.fileExists(atPath: inner.path) else {
            throw ShaderPackImportError.notAShaderPack
        }
        let held = destination.deletingLastPathComponent()
            .appendingPathComponent(".shaderpack-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.moveItem(at: inner, to: held)
        try FileManager.default.removeItem(at: destination)
        try FileManager.default.moveItem(at: held, to: destination)
    }

    /// The archive's own folders above where a companion's presets belong, read off a preset
    /// that names a base pack stage: `x/y/p.slangp` naming `../../../shaders_slang/…` must sit
    /// two folders below its pack folder, so whatever is above those two is wrapping. The
    /// releases have shipped both a bare `retro crisis/` and one under `shaders/shaders_slang/`.
    /// Nil when no preset names a stage, or its folders are too few to reach the depth.
    private nonisolated static func companionWrapping(in tree: URL) -> String? {
        let base = NSRegularExpression.escapedPattern(for: ShaderPresetLibrary.basePackFolderName)
        guard let reference = try? NSRegularExpression(pattern: #"((?:\.\./)+)"# + base + "/") else { return nil }
        let treeDepth = tree.standardizedFileURL.pathComponents.count
        for preset in presetFiles(under: tree).sorted(by: { $0.path < $1.path }) {
            guard let text = try? String(contentsOf: preset, encoding: .utf8),
                  let match = reference.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
                  let ups = Range(match.range(at: 1), in: text) else { continue }
            let keep = text[ups].count / 3 - 1
            let folders = preset.deletingLastPathComponent().standardizedFileURL.pathComponents.dropFirst(treeDepth)
            guard keep >= 0, folders.count >= keep else { return nil }
            return folders.dropLast(keep).joined(separator: "/")
        }
        return nil
    }

    private func install(
        _ source: URL,
        named: String?,
        writing: @escaping @Sendable (URL, URL) throws -> Void
    ) async -> String? {
        let key = source.lastPathComponent
        installing.insert(key)
        errors[key] = nil
        installedName = nil
        installProblem = nil
        var landed: String?
        do {
            let (name, problem) = try await Task.detached(priority: .userInitiated) {
                // A full RetroArch pack is thousands of files, too slow for the main actor.
                try Self.perform(source, named, writing)
            }.value
            landed = name
            installedName = name
            installProblem = problem
        } catch {
            errors[key] = error.localizedDescription
        }
        installing.remove(key)
        return landed
    }

    private nonisolated static func perform(
        _ source: URL,
        _ named: String?,
        _ writing: @Sendable (URL, URL) throws -> Void
    ) throws -> (String, ShaderPresetFailure?) {
        guard let root = ShaderPresetLibrary.prepareUserRoots() else {
            throw ShaderPackImportError.noUserRoot
        }
        let accessing = source.startAccessingSecurityScopedResource()
        defer { if accessing { source.stopAccessingSecurityScopedResource() } }

        let name = freeName(named.map(sanitised) ?? readableName(source), under: root)
        let destination = root.appendingPathComponent(name, isDirectory: true)
        do {
            try writing(source, destination)
        } catch {
            try? FileManager.default.removeItem(at: destination)
            throw error
        }
        let presets = presetFiles(under: destination)
        // A folder with no presets would sit in the browser with nothing to pick, so it is refused.
        guard !presets.isEmpty else {
            try? FileManager.default.removeItem(at: destination)
            throw ShaderPackImportError.notAShaderPack
        }
        return (name, problem(in: presets))
    }

    private nonisolated static func extract(_ source: URL, _ destination: URL) throws {
        let staged = FileManager.default.temporaryDirectory
            .appendingPathComponent("shaderpack-\(UUID().uuidString).zip")
        defer { try? FileManager.default.removeItem(at: staged) }
        try FileManager.default.copyItem(at: source, to: staged)

        var failure: NSError?
        let written = ARMSX2Bridge.extractShaderPackArchive(
            at: staged, to: destination, error: &failure)
        if written.isEmpty {
            if let failure { throw failure }
            throw ShaderPackImportError.notAShaderPack
        }
    }

    /// Copied as is, because a .slangp names its stages by relative path.
    private nonisolated static func copyTree(_ source: URL, _ destination: URL) throws {
        try FileManager.default.copyItem(at: source, to: destination)
    }

    private nonisolated static func freeName(_ base: String, under root: URL) -> String {
        var candidate = base
        var suffix = 2
        while FileManager.default.fileExists(
            atPath: root.appendingPathComponent(candidate, isDirectory: true).path) {
            candidate = "\(base) (\(suffix))"
            suffix += 1
        }
        return candidate
    }

    private nonisolated static func readableName(_ source: URL) -> String {
        var name = source.lastPathComponent
        while name.lowercased().hasSuffix(".zip") {
            name = String(name.dropLast(4))
        }
        return sanitised(name)
    }

    private nonisolated static func sanitised(_ name: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: " _.-"))
        let scalars = name.unicodeScalars.map { allowed.contains($0) ? $0 : Unicode.Scalar("_") }
        let cleaned = String(String.UnicodeScalarView(scalars))
            .trimmingCharacters(in: .whitespaces)
        return cleaned.isEmpty ? "Shader Pack" : cleaned
    }

    private nonisolated static func presetFiles(under directory: URL) -> [URL] {
        guard let walk = FileManager.default.enumerator(
            at: directory, includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]) else { return [] }
        var found: [URL] = []
        while let url = walk.nextObject() as? URL {
            if url.pathExtension.lowercased() == ShaderPresetLibrary.presetExtension {
                found.append(url)
            }
        }
        return found
    }

    /// A pack that fails here stays installed, since it may only need another pack beside it.
    private nonisolated static func problem(in presets: [URL]) -> ShaderPresetFailure? {
        var failure: ShaderPresetFailure?
        for preset in presets.prefix(3) {
            do {
                _ = try ARMSX2Bridge.shaderPresetParameters(atPath: preset.path)
                return nil
            } catch {
                failure = ShaderPresetFailure(error, preset: preset)
            }
        }
        return failure
    }
}
