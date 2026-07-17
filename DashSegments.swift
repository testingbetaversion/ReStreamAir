import Foundation
#if canImport(CoreFoundation)
import CoreFoundation
#endif
#if canImport(FoundationXML)
import FoundationXML
#endif
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
#if os(Linux)
import Glibc
#else
import Darwin
#endif

struct SegmentTemplate {
    var media: String?
    var initialization: String?
    var duration: Double?
    var timescale: Double?
    var startNumber: Int?
    var presentationTimeOffset: Int64?
    var timeline: [TimelineEntry] = []
}

struct TimelineEntry {
    let t: Int64?
    let d: Int64
    let r: Int
    /// DASH-IF low-latency "Segment Sequence Representation" sub-segment
    /// count for this `<S>` entry (e.g. `chunkdurssr` test assets) — when
    /// present and the media template uses `$SubNumber$`, each segment
    /// position is split into `k` independently downloadable/playable CMAF
    /// chunks instead of one whole segment.
    let k: Int?
}

struct Representation {
    var id: String
    var bandwidth: String?
    var codecs: String?
    var width: String?
    var height: String?
    var frameRate: String?
    var audioSamplingRate: String?
    var baseURL: String? = nil
    var template: SegmentTemplate? = nil
}

struct AdaptationSet {
    var id: String?
    var contentType: String?
    var mimeType: String?
    var lang: String?
    var codecs: String?
    var baseURL: String? = nil
    var template: SegmentTemplate? = nil
    var representations: [Representation] = []
}

struct Period {
    var id: String?
    var start: String?
    var baseURL: String? = nil
    var adaptationSets: [AdaptationSet] = []
}

struct MPD {
    var sourceURL: URL
    var type: String?
    var availabilityStartTime: Date?
    var mediaPresentationDuration: Double?
    var minimumUpdatePeriod: Double?
    var timeShiftBufferDepth: Double?
    var baseURL: String?
    var periods: [Period] = []
}

struct SegmentURL: Hashable {
    let periodID: String?
    let adaptationType: String
    let representationID: String
    let isInitialization: Bool
    let number: Int?
    let time: Int64?
    let duration: Double?
    let url: URL
    /// Set only for DASH-IF low-latency CMAF chunks (`$SubNumber$`) — the
    /// original (pre-synthesis) DASH segment number every sibling chunk
    /// shares, so a downstream consumer can tell which chunks belong to the
    /// same segment and must all succeed together or not be served at all
    /// (a decoder can't tolerate a gap in the middle of one GOP the way it
    /// tolerates a whole missing segment).
    let groupKey: Int?
}

struct SegmentInstance {
    let number: Int
    let time: Int64?
    let duration: Double?
    /// Set when this instance is one CMAF chunk (1...k) of a sub-numbered
    /// segment rather than a whole segment — substituted into `$SubNumber$`.
    let subNumber: Int?
}

enum OutputMode {
    case help
    case list
    case info
}

struct FunctionArgumentDoc {
    let name: String
    let required: Bool
    let description: String
}

struct FunctionDoc {
    let name: String
    let purpose: String
    let arguments: [FunctionArgumentDoc]
    let output: String
}

enum DashError: Error, CustomStringConvertible {
    case usage(String)
    case network(String)
    case parse(String)
    case unsupported(String)

    var description: String {
        switch self {
        case .usage(let message), .network(let message), .parse(let message), .unsupported(let message):
            return message
        }
    }
}

struct Options {
    var mode: OutputMode = .help
    var mpdURL: URL?
    var count: Int?
    var fromNumber: Int?
    var includeInitialization = true
    var headers: [(String, String)] = []
    var proxy: String?
    var timeout: TimeInterval = 30
    var representationID: String?
    var videoOnly = false
    var audioOnly = false
    var periodSelector: String?
    var baseURLOverride: URL?
    var json = false
    var sleepRequests: TimeInterval?
}

final class MPDParser: NSObject, XMLParserDelegate {
    private var mpd: MPD
    private let isoFormatter = ISO8601DateFormatter()
    private var currentPeriod: Period?
    private var currentAdaptation: AdaptationSet?
    private var currentRepresentation: Representation?
    private var insideSegmentTimeline = false
    private var currentText = ""

    init(sourceURL: URL) {
        self.mpd = MPD(sourceURL: sourceURL)
        super.init()
        isoFormatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    }

    func parse(data: Data) throws -> MPD {
        let parser = XMLParser(data: data)
        parser.delegate = self
        guard parser.parse() else {
            throw parser.parserError ?? DashError.parse("Could not parse MPD XML.")
        }
        return mpd
    }

    func parser(_ parser: XMLParser,
                didStartElement elementName: String,
                namespaceURI: String?,
                qualifiedName qName: String?,
                attributes attributeDict: [String: String] = [:]) {
        let name = localName(elementName)
        currentText = ""

        switch name {
        case "MPD":
            mpd.type = attributeDict["type"]
            mpd.availabilityStartTime = parseDate(attributeDict["availabilityStartTime"])
            mpd.mediaPresentationDuration = parseDuration(attributeDict["mediaPresentationDuration"])
            mpd.minimumUpdatePeriod = parseDuration(attributeDict["minimumUpdatePeriod"])
            mpd.timeShiftBufferDepth = parseDuration(attributeDict["timeShiftBufferDepth"])

        case "Period":
            currentPeriod = Period(id: attributeDict["id"], start: attributeDict["start"])

        case "AdaptationSet":
            currentAdaptation = AdaptationSet(
                id: attributeDict["id"],
                contentType: attributeDict["contentType"],
                mimeType: attributeDict["mimeType"],
                lang: attributeDict["lang"],
                codecs: attributeDict["codecs"]
            )

        case "Representation":
            currentRepresentation = Representation(
                id: attributeDict["id"] ?? "",
                bandwidth: attributeDict["bandwidth"],
                codecs: attributeDict["codecs"],
                width: attributeDict["width"],
                height: attributeDict["height"],
                frameRate: attributeDict["frameRate"],
                audioSamplingRate: attributeDict["audioSamplingRate"]
            )

        case "SegmentTemplate":
            let template = SegmentTemplate(
                media: attributeDict["media"],
                initialization: attributeDict["initialization"],
                duration: double(attributeDict["duration"]),
                timescale: double(attributeDict["timescale"]),
                startNumber: int(attributeDict["startNumber"]),
                presentationTimeOffset: int64(attributeDict["presentationTimeOffset"])
            )
            if currentRepresentation != nil {
                currentRepresentation?.template = template
            } else if currentAdaptation != nil {
                currentAdaptation?.template = template
            }

        case "SegmentTimeline":
            insideSegmentTimeline = true

        case "S":
            guard insideSegmentTimeline else { return }
            appendTimeline(TimelineEntry(
                t: int64(attributeDict["t"]),
                d: int64(attributeDict["d"]) ?? 0,
                r: int(attributeDict["r"]) ?? 0,
                k: int(attributeDict["k"])
            ))

        default:
            break
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        currentText += string
    }

    func parser(_ parser: XMLParser,
                didEndElement elementName: String,
                namespaceURI: String?,
                qualifiedName qName: String?) {
        let name = localName(elementName)
        switch name {
        case "BaseURL":
            let value = currentText.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !value.isEmpty else { break }
            if currentRepresentation != nil {
                currentRepresentation?.baseURL = value
            } else if currentAdaptation != nil {
                currentAdaptation?.baseURL = value
            } else if currentPeriod != nil {
                currentPeriod?.baseURL = value
            } else {
                mpd.baseURL = value
            }

        case "Representation":
            if let representation = currentRepresentation {
                currentAdaptation?.representations.append(representation)
            }
            currentRepresentation = nil

        case "AdaptationSet":
            if let adaptation = currentAdaptation {
                currentPeriod?.adaptationSets.append(adaptation)
            }
            currentAdaptation = nil

        case "Period":
            if let period = currentPeriod {
                mpd.periods.append(period)
            }
            currentPeriod = nil

        case "SegmentTimeline":
            insideSegmentTimeline = false

        default:
            break
        }
        currentText = ""
    }

    private func appendTimeline(_ entry: TimelineEntry) {
        if currentRepresentation?.template != nil {
            currentRepresentation?.template?.timeline.append(entry)
        } else {
            currentAdaptation?.template?.timeline.append(entry)
        }
    }

    private func localName(_ name: String) -> String {
        name.split(separator: ":").last.map(String.init) ?? name
    }

    private func parseDate(_ text: String?) -> Date? {
        guard let text else { return nil }
        if let date = isoFormatter.date(from: text) {
            return date
        }
        let fallback = ISO8601DateFormatter()
        fallback.formatOptions = [.withInternetDateTime]
        return fallback.date(from: text)
    }
}

final class XMLTreeNode {
    let name: String
    let attributes: [String: String]
    var text = ""
    var children: [XMLTreeNode] = []

    init(name: String, attributes: [String: String]) {
        self.name = name
        self.attributes = attributes
    }
}

final class XMLTreeParser: NSObject, XMLParserDelegate {
    private var root: XMLTreeNode?
    private var stack: [XMLTreeNode] = []

    func parse(data: Data) throws -> XMLTreeNode {
        let parser = XMLParser(data: data)
        parser.delegate = self
        guard parser.parse(), let root else {
            throw parser.parserError ?? DashError.parse("Could not parse MPD XML tree.")
        }
        return root
    }

    func parser(_ parser: XMLParser,
                didStartElement elementName: String,
                namespaceURI: String?,
                qualifiedName qName: String?,
                attributes attributeDict: [String: String] = [:]) {
        let node = XMLTreeNode(name: localName(elementName), attributes: attributeDict)
        if let parent = stack.last {
            parent.children.append(node)
        } else {
            root = node
        }
        stack.append(node)
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        stack.last?.text += string
    }

    func parser(_ parser: XMLParser,
                didEndElement elementName: String,
                namespaceURI: String?,
                qualifiedName qName: String?) {
        _ = stack.popLast()
    }

    private func localName(_ name: String) -> String {
        name.split(separator: ":").last.map(String.init) ?? name
    }
}

struct FunctionCall {
    let name: String
    let args: [String: String]
}

struct ActionRuntime {
    static let functions: [FunctionDoc] = [
        FunctionDoc(
            name: "describeFunctions",
            purpose: "Return the exported Swift actions, their arguments, and their JSON output contract.",
            arguments: [],
            output: "JSON object with a functions array."
        ),
        FunctionDoc(
            name: "info",
            purpose: "Fetch and parse an MPD, then return periods, adaptation sets, and representations.",
            arguments: commonMPDArguments() + [
                FunctionArgumentDoc(name: "period", required: false, description: "Period id or zero-based index."),
                FunctionArgumentDoc(name: "representation", required: false, description: "Representation@id filter."),
                FunctionArgumentDoc(name: "videoOnly", required: false, description: "true to include only video adaptation sets."),
                FunctionArgumentDoc(name: "audioOnly", required: false, description: "true to include only audio adaptation sets.")
            ],
            output: "JSON object matching --info --json."
        ),
        FunctionDoc(
            name: "readMPD",
            purpose: "Read an MPD with customizable output: parsed DASH model, raw XML tree, selected elements, expanded segments, or raw XML text.",
            arguments: commonMPDArguments() + [
                FunctionArgumentDoc(name: "file", required: false, description: "Local MPD file path. Used when mpd is not supplied."),
                FunctionArgumentDoc(name: "includeModel", required: false, description: "true or false. Include parsed DASH model. Defaults to true."),
                FunctionArgumentDoc(name: "includeTree", required: false, description: "true or false. Include full XML tree. Defaults to false."),
                FunctionArgumentDoc(name: "elements", required: false, description: "Comma-separated XML element names to return as a flat list, for example Representation,SegmentTemplate,S."),
                FunctionArgumentDoc(name: "attributes", required: false, description: "Comma-separated attribute names to include, or all. Defaults to all."),
                FunctionArgumentDoc(name: "includeAttributes", required: false, description: "true or false. Include XML attributes. Defaults to true."),
                FunctionArgumentDoc(name: "includeText", required: false, description: "true or false. Include trimmed XML text values. Defaults to false."),
                FunctionArgumentDoc(name: "maxDepth", required: false, description: "Maximum XML tree depth when includeTree=true."),
                FunctionArgumentDoc(name: "includeSegments", required: false, description: "true or false. Include expanded segment URLs. Defaults to false."),
                FunctionArgumentDoc(name: "includeRawXML", required: false, description: "true or false. Include original MPD XML text. Defaults to false."),
                FunctionArgumentDoc(name: "period", required: false, description: "Period id or zero-based index for model and segment output."),
                FunctionArgumentDoc(name: "representation", required: false, description: "Representation@id filter for model and segment output."),
                FunctionArgumentDoc(name: "videoOnly", required: false, description: "true to include only video adaptation sets."),
                FunctionArgumentDoc(name: "audioOnly", required: false, description: "true to include only audio adaptation sets."),
                FunctionArgumentDoc(name: "count", required: false, description: "Maximum media segments per representation when includeSegments=true."),
                FunctionArgumentDoc(name: "from", required: false, description: "Starting DASH segment number when includeSegments=true."),
                FunctionArgumentDoc(name: "includeInit", required: false, description: "true or false. Include initialization URLs when includeSegments=true. Defaults to true."),
                FunctionArgumentDoc(name: "baseUrl", required: false, description: "Override base URL used to resolve segment paths.")
            ],
            output: "JSON object with source, optional model, optional tree, optional elements, optional segments, and optional rawXML."
        ),
        FunctionDoc(
            name: "listSegments",
            purpose: "Fetch and parse an MPD, expand matching SegmentTemplate entries, and return segment URLs.",
            arguments: commonMPDArguments() + [
                FunctionArgumentDoc(name: "period", required: false, description: "Period id or zero-based index."),
                FunctionArgumentDoc(name: "representation", required: false, description: "Representation@id filter."),
                FunctionArgumentDoc(name: "videoOnly", required: false, description: "true to include only video adaptation sets."),
                FunctionArgumentDoc(name: "audioOnly", required: false, description: "true to include only audio adaptation sets."),
                FunctionArgumentDoc(name: "count", required: false, description: "Maximum media segments per representation."),
                FunctionArgumentDoc(name: "from", required: false, description: "Starting DASH segment number."),
                FunctionArgumentDoc(name: "includeInit", required: false, description: "true or false. Defaults to true."),
                FunctionArgumentDoc(name: "baseUrl", required: false, description: "Override base URL for resolving segment paths.")
            ],
            output: "JSON object with mpd, type, and segments array."
        ),
        FunctionDoc(
            name: "parseDuration",
            purpose: "Parse an ISO 8601 DASH duration string into seconds.",
            arguments: [
                FunctionArgumentDoc(name: "value", required: true, description: "Duration string such as PT2S or PT1M30.5S.")
            ],
            output: "JSON object with seconds as a number or null when parsing fails."
        ),
        FunctionDoc(
            name: "fillTemplate",
            purpose: "Replace DASH SegmentTemplate tokens with supplied representation, bandwidth, number, and time values.",
            arguments: [
                FunctionArgumentDoc(name: "template", required: true, description: "Template string containing tokens like $RepresentationID$, $Number$, or $Time$."),
                FunctionArgumentDoc(name: "representation", required: false, description: "Value for $RepresentationID$. Defaults to empty string."),
                FunctionArgumentDoc(name: "bandwidth", required: false, description: "Value for $Bandwidth$. Defaults to empty string."),
                FunctionArgumentDoc(name: "number", required: false, description: "Integer value for $Number$ tokens."),
                FunctionArgumentDoc(name: "time", required: false, description: "Integer value for $Time$ tokens.")
            ],
            output: "JSON object with the filled template string."
        )
    ]

    static func canHandle(_ arguments: [String]) -> Bool {
        arguments.contains("--action")
            || arguments.contains(where: { $0.hasPrefix("action=") || $0.hasPrefix("function=") })
    }

    static func run(_ arguments: [String]) {
        do {
            let call = try parseCall(arguments)
            let result = try execute(call)
            DashSegmentsCLI.printJSON([
                "ok": true,
                "action": call.name,
                "result": result
            ])
        } catch {
            DashSegmentsCLI.printJSON([
                "ok": false,
                "error": "\(error)"
            ])
            exit(1)
        }
    }

    static func parseCall(_ arguments: [String]) throws -> FunctionCall {
        var name: String?
        var args: [String: String] = [:]
        var index = 0

        while index < arguments.count {
            let argument = arguments[index]
            if argument == "--action" {
                index += 1
                guard index < arguments.count else {
                    throw DashError.usage("--action requires a function name.")
                }
                name = arguments[index]
            } else if argument == "--arg" {
                index += 1
                guard index < arguments.count else {
                    throw DashError.usage("--arg requires key=value.")
                }
                try addKeyValue(arguments[index], to: &args, name: &name)
            } else if argument.contains("="), !argument.hasPrefix("-") {
                try addKeyValue(argument, to: &args, name: &name)
            } else {
                throw DashError.usage("Action mode only accepts --action NAME, --arg key=value, or key=value arguments.")
            }
            index += 1
        }

        guard let name, !name.isEmpty else {
            throw DashError.usage("Action mode requires action=NAME, function=NAME, or --action NAME.")
        }
        return FunctionCall(name: name, args: args)
    }

    static func addKeyValue(_ text: String, to args: inout [String: String], name: inout String?) throws {
        guard let separator = text.firstIndex(of: "=") else {
            throw DashError.usage("Expected key=value, got '\(text)'.")
        }
        let key = String(text[..<separator])
        let value = String(text[text.index(after: separator)...])
        guard !key.isEmpty else {
            throw DashError.usage("Argument key cannot be empty.")
        }
        if key == "action" || key == "function" {
            name = value
        } else {
            args[key] = value
        }
    }

    static func execute(_ call: FunctionCall) throws -> Any {
        switch call.name {
        case "describeFunctions":
            return ["functions": functions.map(functionDocJSON)]
        case "info":
            let options = try options(from: call.args, mode: .info)
            let mpd = try DashSegmentsCLI.loadMPD(options: options)
            let periods = try DashSegmentsCLI.selectedPeriods(mpd: mpd, options: options)
            try DashSegmentsCLI.ensureRepresentationExists(in: periods, options: options)
            return DashSegmentsCLI.infoJSON(mpd: mpd, periods: periods, options: options)
        case "readMPD":
            return try readMPD(call.args)
        case "listSegments":
            let options = try options(from: call.args, mode: .list)
            let mpd = try DashSegmentsCLI.loadMPD(options: options)
            let segments = try DashSegmentsCLI.expandSegments(mpd: mpd, options: options)
            return [
                "mpd": mpd.sourceURL.absoluteString,
                "type": mpd.type ?? "static",
                "segments": segments.map(DashSegmentsCLI.segmentJSON)
            ]
        case "parseDuration":
            let value = try required("value", in: call.args)
            return [
                "value": value,
                "seconds": DashSegmentsCLI.optionalAny(parseDuration(value))
            ]
        case "fillTemplate":
            let template = try required("template", in: call.args)
            let representation = Representation(
                id: call.args["representation"] ?? "",
                bandwidth: call.args["bandwidth"],
                codecs: nil,
                width: nil,
                height: nil,
                frameRate: nil,
                audioSamplingRate: nil
            )
            let number = try optionalInt("number", in: call.args)
            let time = try optionalInt64("time", in: call.args)
            return [
                "template": template,
                "value": DashSegmentsCLI.fillTemplate(template, representation: representation, number: number, time: time)
            ]
        default:
            throw DashError.usage("Unknown action '\(call.name)'. Use action=describeFunctions to list exported actions.")
        }
    }

    static func readMPD(_ args: [String: String]) throws -> [String: Any] {
        var options = try options(from: args, mode: .info, allowFile: true)
        let source = try mpdSource(from: args)
        let data: Data
        switch source {
        case .remote(let url):
            options.mpdURL = url
            data = try DashSegmentsCLI.fetch(url, options: options)
        case .file(let url):
            options.mpdURL = url
            data = try Data(contentsOf: url)
        }

        let mpd = try MPDParser(sourceURL: options.mpdURL ?? source.url).parse(data: data)
        let root = try XMLTreeParser().parse(data: data)
        let periods = try DashSegmentsCLI.selectedPeriods(mpd: mpd, options: options)
        try DashSegmentsCLI.ensureRepresentationExists(in: periods, options: options)

        let includeModel = try optionalBool("includeModel", in: args) ?? true
        let includeTree = try optionalBool("includeTree", in: args) ?? false
        let includeAttributes = try optionalBool("includeAttributes", in: args) ?? true
        let includeText = try optionalBool("includeText", in: args) ?? false
        let includeSegments = try optionalBool("includeSegments", in: args) ?? false
        let includeRawXML = try optionalBool("includeRawXML", in: args) ?? false
        let elementNames = csvSet(args["elements"])
        let attributeNames = csvSet(args["attributes"])
        let maxDepth = try optionalPositiveInt("maxDepth", in: args)

        var output: [String: Any] = [
            "source": source.description,
            "sourceType": source.type,
            "bytes": data.count
        ]

        if includeModel {
            output["model"] = DashSegmentsCLI.infoJSON(mpd: mpd, periods: periods, options: options)
        }
        if includeTree {
            output["tree"] = xmlNodeJSON(
                root,
                includeAttributes: includeAttributes,
                attributeNames: attributeNames,
                includeText: includeText,
                maxDepth: maxDepth,
                depth: 0
            )
        }
        if let elementNames {
            output["elements"] = collectXMLNodes(
                root,
                names: elementNames,
                includeAttributes: includeAttributes,
                attributeNames: attributeNames,
                includeText: includeText,
                maxDepth: maxDepth
            )
        }
        if includeSegments {
            let segments = try DashSegmentsCLI.expandSegments(mpd: mpd, options: options)
            output["segments"] = segments.map(DashSegmentsCLI.segmentJSON)
        }
        if includeRawXML {
            output["rawXML"] = String(data: data, encoding: .utf8) ?? ""
        }
        return output
    }

    enum MPDSource {
        case remote(URL)
        case file(URL)

        var url: URL {
            switch self {
            case .remote(let url), .file(let url): return url
            }
        }

        var description: String { url.absoluteString }
        var type: String {
            switch self {
            case .remote: return "url"
            case .file: return "file"
            }
        }
    }

    static func mpdSource(from args: [String: String]) throws -> MPDSource {
        if let mpd = args["mpd"], !mpd.isEmpty {
            guard let url = URL(string: mpd), url.scheme != nil else {
                throw DashError.usage("mpd must be an absolute URL.")
            }
            return .remote(url)
        }
        if let file = args["file"], !file.isEmpty {
            return .file(URL(fileURLWithPath: file))
        }
        throw DashError.usage("Missing required argument 'mpd' or 'file'.")
    }

    static func options(from args: [String: String], mode: OutputMode, allowFile: Bool = false) throws -> Options {
        var options = Options(mode: mode)
        let source = try mpdSource(from: args)
        switch source {
        case .remote(let url):
            options.mpdURL = url
        case .file(let url):
            guard allowFile else {
                throw DashError.usage("mpd must be an absolute URL.")
            }
            options.mpdURL = url
        }
        options.json = true
        options.timeout = try optionalDouble("timeout", in: args) ?? options.timeout
        options.representationID = args["representation"] ?? args["representationID"]
        options.periodSelector = args["period"]
        options.videoOnly = try optionalBool("videoOnly", in: args) ?? false
        options.audioOnly = try optionalBool("audioOnly", in: args) ?? false
        options.count = try optionalPositiveInt("count", in: args)
        options.fromNumber = try optionalNonNegativeInt("from", in: args)
        options.includeInitialization = try optionalBool("includeInit", in: args) ?? true
        options.proxy = args["proxy"]

        if let baseUrl = args["baseUrl"] ?? args["baseURL"] {
            guard let url = URL(string: baseUrl), url.scheme != nil else {
                throw DashError.usage("baseUrl must be an absolute URL.")
            }
            options.baseURLOverride = url
        }

        if let headerText = args["header"] {
            options.headers = try parseHeaders(headerText)
        }

        if options.videoOnly && options.audioOnly {
            throw DashError.usage("Use either videoOnly=true or audioOnly=true, not both.")
        }
        return options
    }

    static func xmlNodeJSON(_ node: XMLTreeNode,
                            includeAttributes: Bool,
                            attributeNames: Set<String>?,
                            includeText: Bool,
                            maxDepth: Int?,
                            depth: Int) -> [String: Any] {
        var output: [String: Any] = ["name": node.name]
        if includeAttributes {
            output["attributes"] = filteredAttributes(node.attributes, names: attributeNames)
        }
        if includeText {
            let text = node.text.trimmingCharacters(in: .whitespacesAndNewlines)
            if !text.isEmpty {
                output["text"] = text
            }
        }
        if maxDepth == nil || depth < (maxDepth ?? 0) {
            output["children"] = node.children.map {
                xmlNodeJSON(
                    $0,
                    includeAttributes: includeAttributes,
                    attributeNames: attributeNames,
                    includeText: includeText,
                    maxDepth: maxDepth,
                    depth: depth + 1
                )
            }
        }
        return output
    }

    static func collectXMLNodes(_ node: XMLTreeNode,
                                names: Set<String>,
                                includeAttributes: Bool,
                                attributeNames: Set<String>?,
                                includeText: Bool,
                                maxDepth: Int?) -> [[String: Any]] {
        var matches: [[String: Any]] = []
        collectXMLNodes(
            node,
            names: names,
            includeAttributes: includeAttributes,
            attributeNames: attributeNames,
            includeText: includeText,
            maxDepth: maxDepth,
            depth: 0,
            matches: &matches
        )
        return matches
    }

    static func collectXMLNodes(_ node: XMLTreeNode,
                                names: Set<String>,
                                includeAttributes: Bool,
                                attributeNames: Set<String>?,
                                includeText: Bool,
                                maxDepth: Int?,
                                depth: Int,
                                matches: inout [[String: Any]]) {
        if names.contains(node.name) {
            matches.append(xmlNodeJSON(
                node,
                includeAttributes: includeAttributes,
                attributeNames: attributeNames,
                includeText: includeText,
                maxDepth: maxDepth,
                depth: 0
            ))
        }
        for child in node.children {
            collectXMLNodes(
                child,
                names: names,
                includeAttributes: includeAttributes,
                attributeNames: attributeNames,
                includeText: includeText,
                maxDepth: maxDepth,
                depth: depth + 1,
                matches: &matches
            )
        }
    }

    static func filteredAttributes(_ attributes: [String: String], names: Set<String>?) -> [String: String] {
        guard let names, !names.contains("all") else { return attributes }
        return attributes.filter { names.contains($0.key) }
    }

    static func csvSet(_ value: String?) -> Set<String>? {
        guard let value, !value.isEmpty else { return nil }
        let names = value.split(separator: ",").map {
            String($0).trimmingCharacters(in: .whitespacesAndNewlines)
        }.filter { !$0.isEmpty }
        return names.isEmpty ? nil : Set(names)
    }

    static func parseHeaders(_ text: String) throws -> [(String, String)] {
        try text.split(separator: "|").map { item in
            guard let separator = item.firstIndex(of: ":") else {
                throw DashError.usage("header must look like 'Name: value' or 'Name: value|Name2: value2'.")
            }
            let name = item[..<separator].trimmingCharacters(in: .whitespaces)
            let value = item[item.index(after: separator)...].trimmingCharacters(in: .whitespaces)
            guard !name.isEmpty else {
                throw DashError.usage("header name cannot be empty.")
            }
            return (String(name), String(value))
        }
    }

    static func functionDocJSON(_ doc: FunctionDoc) -> [String: Any] {
        [
            "name": doc.name,
            "purpose": doc.purpose,
            "arguments": doc.arguments.map {
                [
                    "name": $0.name,
                    "required": $0.required,
                    "description": $0.description
                ]
            },
            "output": doc.output
        ]
    }

    static func commonMPDArguments() -> [FunctionArgumentDoc] {
        [
            FunctionArgumentDoc(name: "mpd", required: true, description: "Absolute MPD URL."),
            FunctionArgumentDoc(name: "timeout", required: false, description: "HTTP timeout in seconds. Defaults to 30."),
            FunctionArgumentDoc(name: "header", required: false, description: "HTTP header string. Separate multiple headers with |."),
            FunctionArgumentDoc(name: "proxy", required: false, description: "HTTP or HTTPS proxy URL.")
        ]
    }

    static func required(_ key: String, in args: [String: String]) throws -> String {
        guard let value = args[key], !value.isEmpty else {
            throw DashError.usage("Missing required argument '\(key)'.")
        }
        return value
    }

    static func optionalBool(_ key: String, in args: [String: String]) throws -> Bool? {
        guard let value = args[key] else { return nil }
        switch value.lowercased() {
        case "true", "1", "yes": return true
        case "false", "0", "no": return false
        default: throw DashError.usage("\(key) must be true or false.")
        }
    }

    static func optionalInt(_ key: String, in args: [String: String]) throws -> Int? {
        guard let value = args[key] else { return nil }
        guard let number = Int(value) else {
            throw DashError.usage("\(key) must be an integer.")
        }
        return number
    }

    static func optionalInt64(_ key: String, in args: [String: String]) throws -> Int64? {
        guard let value = args[key] else { return nil }
        guard let number = Int64(value) else {
            throw DashError.usage("\(key) must be an integer.")
        }
        return number
    }

    static func optionalPositiveInt(_ key: String, in args: [String: String]) throws -> Int? {
        guard let number = try optionalInt(key, in: args) else { return nil }
        guard number > 0 else {
            throw DashError.usage("\(key) must be greater than zero.")
        }
        return number
    }

    static func optionalNonNegativeInt(_ key: String, in args: [String: String]) throws -> Int? {
        guard let number = try optionalInt(key, in: args) else { return nil }
        guard number >= 0 else {
            throw DashError.usage("\(key) must be zero or greater.")
        }
        return number
    }

    static func optionalDouble(_ key: String, in args: [String: String]) throws -> Double? {
        guard let value = args[key] else { return nil }
        guard let number = Double(value), number > 0 else {
            throw DashError.usage("\(key) must be a number greater than zero.")
        }
        return number
    }
}

struct DashSegmentsCLI {
    static func main(_ arguments: [String]) {
        do {
            if ActionRuntime.canHandle(arguments) {
                ActionRuntime.run(arguments)
                return
            }

            let options = try parseOptions(arguments)
            guard options.mode != .help else {
                print(usage())
                return
            }
            guard options.mpdURL != nil else {
                throw DashError.usage("MPD_URL is required.")
            }

            switch options.mode {
            case .list:
                try runList(options: options)
            case .info:
                let mpd = try loadMPD(options: options)
                printInfo(mpd: mpd, options: options)
            case .help:
                print(usage())
            }
        } catch {
            fputs("error: \(error)\n\n\(usage())\n", stderr)
            exit(1)
        }
    }

    static func parseOptions(_ args: [String]) throws -> Options {
        var options = Options()
        var i = 0

        func requireValue(_ flag: String) throws -> String {
            i += 1
            guard i < args.count else {
                throw DashError.usage("\(flag) requires a value.")
            }
            return args[i]
        }

        while i < args.count {
            let arg = args[i]
            switch arg {
            case "--help", "-h":
                options.mode = .help

            case "--list":
                options.mode = .list

            case "--info":
                options.mode = .info

            case "--json":
                options.json = true

            case "--count":
                let value = try requireValue(arg)
                guard let count = Int(value), count > 0 else {
                    throw DashError.usage("--count requires a positive integer.")
                }
                options.count = count

            case "--from":
                let value = try requireValue(arg)
                guard let number = Int(value), number >= 0 else {
                    throw DashError.usage("--from requires a non-negative segment number.")
                }
                options.fromNumber = number

            case "--no-init":
                options.includeInitialization = false

            case "--include-init":
                options.includeInitialization = true

            case "-H", "--header":
                let header = try requireValue(arg)
                guard let separator = header.firstIndex(of: ":") else {
                    throw DashError.usage("\(arg) must look like 'Name: value'.")
                }
                let name = header[..<separator].trimmingCharacters(in: .whitespaces)
                let value = header[header.index(after: separator)...].trimmingCharacters(in: .whitespaces)
                guard !name.isEmpty else {
                    throw DashError.usage("\(arg) header name cannot be empty.")
                }
                options.headers.append((String(name), String(value)))

            case "--proxy":
                options.proxy = try requireValue(arg)

            case "--timeout":
                let value = try requireValue(arg)
                guard let timeout = Double(value), timeout > 0 else {
                    throw DashError.usage("--timeout requires seconds greater than zero.")
                }
                options.timeout = timeout

            case "--representation", "--representation-id":
                options.representationID = try requireValue(arg)

            case "--video-only":
                options.videoOnly = true

            case "--audio-only":
                options.audioOnly = true

            case "--period":
                options.periodSelector = try requireValue(arg)

            case "--base-url":
                let value = try requireValue(arg)
                guard let url = URL(string: value), url.scheme != nil else {
                    throw DashError.usage("--base-url requires an absolute URL.")
                }
                options.baseURLOverride = url

            case "--sleep-requests":
                let value = try requireValue(arg)
                guard let seconds = Double(value), seconds > 0 else {
                    throw DashError.usage("--sleep-requests requires seconds greater than zero.")
                }
                options.sleepRequests = seconds

            default:
                guard !arg.hasPrefix("-") else {
                    throw DashError.usage("Unknown option: \(arg)")
                }
                guard options.mpdURL == nil else {
                    throw DashError.usage("Only one MPD_URL is supported per run.")
                }
                guard let url = URL(string: arg), url.scheme != nil else {
                    throw DashError.usage("Invalid MPD_URL: \(arg)")
                }
                options.mpdURL = url
            }
            i += 1
        }

        if options.videoOnly && options.audioOnly {
            throw DashError.usage("Use either --video-only or --audio-only, not both.")
        }
        return options
    }

    static func runList(options: Options) throws {
        var seen = Set<SegmentURL>()
        var firstPass = true

        while true {
            let mpd = try loadMPD(options: options)
            let segments = try expandSegments(mpd: mpd, options: options)
            let fresh = segments.filter { !seen.contains($0) }
            fresh.forEach { seen.insert($0) }

            if options.json {
                printJSON([
                    "mpd": mpd.sourceURL.absoluteString,
                    "type": mpd.type ?? "static",
                    "segments": fresh.map(segmentJSON)
                ])
            } else {
                for segment in fresh {
                    print(segment.url.absoluteString)
                }
            }
            fflush(stdout)

            guard mpd.type == "dynamic", let sleep = options.sleepRequests else {
                break
            }
            if firstPass && fresh.isEmpty {
                fputs("warning: no segments matched filters; still polling live MPD\n", stderr)
            }
            firstPass = false
            Thread.sleep(forTimeInterval: sleep)
        }
    }

    static func loadMPD(options: Options) throws -> MPD {
        guard let url = options.mpdURL else {
            throw DashError.usage("MPD_URL is required.")
        }
        let data = try fetch(url, options: options)
        return try MPDParser(sourceURL: url).parse(data: data)
    }

    static func fetch(_ url: URL, options: Options) throws -> Data {
        try HTTPClient(options: httpOptions(from: options)).get(url)
    }

    static func httpOptions(from options: Options) -> HTTPRequestOptions {
        HTTPRequestOptions(
            timeout: options.timeout,
            headers: options.headers,
            proxy: options.proxy
        )
    }

    static func selectedPeriods(mpd: MPD, options: Options) throws -> [Period] {
        guard let selector = options.periodSelector else {
            return mpd.periods
        }
        if let index = Int(selector) {
            guard index >= 0, index < mpd.periods.count else {
                throw DashError.usage("--period index \(index) is out of range.")
            }
            return [mpd.periods[index]]
        }
        let matches = mpd.periods.filter { $0.id == selector }
        guard !matches.isEmpty else {
            throw DashError.usage("No period found with id '\(selector)'.")
        }
        return matches
    }

    static func adaptationType(_ adaptation: AdaptationSet) -> String {
        if let contentType = adaptation.contentType {
            return contentType
        }
        if let mimeType = adaptation.mimeType {
            if mimeType.contains("video") { return "video" }
            if mimeType.contains("audio") { return "audio" }
            return mimeType
        }
        // Some MPDs (e.g. Axinom's public clearkey test vectors) omit both
        // contentType and mimeType on AdaptationSet entirely. Fall back to
        // the first representation's dimensions/codec string.
        if let representation = adaptation.representations.first {
            if representation.width != nil || representation.height != nil { return "video" }
            if let codecs = (representation.codecs ?? adaptation.codecs)?.lowercased() {
                let videoCodecs = ["avc1", "avc3", "hvc1", "hev1", "vp8", "vp9", "av01"]
                let audioCodecs = ["mp4a", "ac-3", "ec-3", "opus", "vorbis", "flac"]
                if videoCodecs.contains(where: { codecs.hasPrefix($0) }) { return "video" }
                if audioCodecs.contains(where: { codecs.hasPrefix($0) }) { return "audio" }
            }
        }
        return "unknown"
    }

    static func passesFilters(adaptation: AdaptationSet, representation: Representation, options: Options) -> Bool {
        let type = adaptationType(adaptation)
        if options.videoOnly && type != "video" { return false }
        if options.audioOnly && type != "audio" { return false }
        if let id = options.representationID, representation.id != id { return false }
        return true
    }

    static func passesTypeFilters(adaptation: AdaptationSet, options: Options) -> Bool {
        let type = adaptationType(adaptation)
        if options.videoOnly && type != "video" { return false }
        if options.audioOnly && type != "audio" { return false }
        return true
    }

    static func matchingRepresentations(adaptation: AdaptationSet, options: Options) -> [Representation] {
        adaptation.representations.filter {
            passesFilters(adaptation: adaptation, representation: $0, options: options)
        }
    }

    static func ensureRepresentationExists(in periods: [Period], options: Options) throws {
        guard let id = options.representationID else { return }
        let found = periods.contains { period in
            period.adaptationSets.contains { adaptation in
                adaptation.representations.contains { $0.id == id }
            }
        }
        if !found {
            throw DashError.usage("No representation found with id '\(id)'.")
        }
    }

    static func expandSegments(mpd: MPD, options: Options) throws -> [SegmentURL] {
        let periods = try selectedPeriods(mpd: mpd, options: options)

        // A dynamic (live) MPD with SCTE-35 ad insertion splits its timeline
        // into several Periods (a new one at every ad break). The `count`
        // (downloadAhead) limit has to bound the *combined* live window across
        // those periods, not each one independently: applying it per period
        // silently drops the tail of every period longer than `count`, which
        // both loses those segments outright and — because the next period then
        // starts a dozen seconds later in media time — forces a bogus
        // EXT-X-DISCONTINUITY at each boundary. That is the "stutters/rebuffers
        // every few seconds" symptom on multi-period clearkey/DRM streams.
        //
        // So for dynamic MPDs we gather each representation's segments across
        // all periods unbounded, then keep the newest `count` of that
        // contiguous sequence — the live edge, matching what the duration-based
        // branch of `segmentInstances` already does for timeline-less dynamic
        // manifests. Static/VOD keeps the historical per-period behaviour
        // (there `count` is only a debug limiter).
        if mpd.type == "dynamic" {
            return try expandDynamicSegments(mpd: mpd, periods: periods, options: options)
        }

        var output: [SegmentURL] = []
        var sawRepresentationID = options.representationID == nil

        for period in periods {
            for adaptation in period.adaptationSets {
                for representation in adaptation.representations {
                    if let id = options.representationID, representation.id == id {
                        sawRepresentationID = true
                    }
                    guard passesFilters(adaptation: adaptation, representation: representation, options: options) else {
                        continue
                    }
                    guard let template = mergedTemplate(adaptation.template, representation.template),
                          let media = template.media else {
                        continue
                    }

                    let base = representationBaseURL(
                        mpd: mpd,
                        period: period,
                        adaptation: adaptation,
                        representation: representation,
                        override: options.baseURLOverride
                    )
                    let type = adaptationType(adaptation)

                    if options.includeInitialization, let initialization = template.initialization {
                        let path = fillTemplate(initialization, representation: representation, number: nil, time: nil)
                        if let url = URL(string: path, relativeTo: base)?.absoluteURL {
                            output.append(SegmentURL(
                                periodID: period.id,
                                adaptationType: type,
                                representationID: representation.id,
                                isInitialization: true,
                                number: nil,
                                time: nil,
                                duration: nil,
                                url: url,
                                groupKey: nil
                            ))
                        }
                    }

                    let instances = try segmentInstances(mpd: mpd, template: template, options: options)
                    for instance in instances {
                        let path = fillTemplate(media, representation: representation, number: instance.number, time: instance.time, subNumber: instance.subNumber)
                        guard let url = URL(string: path, relativeTo: base)?.absoluteURL else { continue }
                        // Sort/dedup key used everywhere downstream (playlist
                        // media sequence, local filenames, direct-stream
                        // tailing) — folds the chunk index in so sub-numbered
                        // CMAF chunks stay ordered instead of colliding on
                        // their shared DASH segment number.
                        let sortNumber = instance.subNumber.map { instance.number * 1000 + $0 } ?? instance.number
                        output.append(SegmentURL(
                            periodID: period.id,
                            adaptationType: type,
                            representationID: representation.id,
                            isInitialization: false,
                            number: sortNumber,
                            time: instance.time,
                            duration: instance.duration,
                            url: url,
                            groupKey: instance.subNumber != nil ? instance.number : nil
                        ))
                    }
                }
            }
        }

        if !sawRepresentationID, let id = options.representationID {
            throw DashError.usage("No representation found with id '\(id)'.")
        }
        guard !output.isEmpty else {
            throw DashError.unsupported("No segments matched this MPD and filter set.")
        }
        return output
    }

    /// Segment expansion for dynamic (live) MPDs. Each representation's media
    /// segments are collected across every selected period into one contiguous
    /// timeline; the `count` limit is then applied once, to the newest end of
    /// that combined sequence (the live edge). This is what keeps ad-break
    /// period boundaries gapless — see the note in `expandSegments`.
    private static func expandDynamicSegments(mpd: MPD, periods: [Period], options: Options) throws -> [SegmentURL] {
        var sawRepresentationID = options.representationID == nil
        // Representation keys in first-seen order, so init lines still precede
        // their media and video/audio stay grouped the way the single-period
        // path emitted them.
        var order: [String] = []
        var initByKey: [String: [SegmentURL]] = [:]
        var mediaByKey: [String: [SegmentURL]] = [:]

        for period in periods {
            for adaptation in period.adaptationSets {
                for representation in adaptation.representations {
                    if let id = options.representationID, representation.id == id {
                        sawRepresentationID = true
                    }
                    guard passesFilters(adaptation: adaptation, representation: representation, options: options) else {
                        continue
                    }
                    guard let template = mergedTemplate(adaptation.template, representation.template),
                          let media = template.media else {
                        continue
                    }

                    let base = representationBaseURL(
                        mpd: mpd,
                        period: period,
                        adaptation: adaptation,
                        representation: representation,
                        override: options.baseURLOverride
                    )
                    let type = adaptationType(adaptation)
                    let key = "\(type)\u{1}\(representation.id)"
                    if initByKey[key] == nil && mediaByKey[key] == nil { order.append(key) }

                    if options.includeInitialization, let initialization = template.initialization {
                        let path = fillTemplate(initialization, representation: representation, number: nil, time: nil)
                        if let url = URL(string: path, relativeTo: base)?.absoluteURL {
                            initByKey[key, default: []].append(SegmentURL(
                                periodID: period.id,
                                adaptationType: type,
                                representationID: representation.id,
                                isInitialization: true,
                                number: nil,
                                time: nil,
                                duration: nil,
                                url: url,
                                groupKey: nil
                            ))
                        }
                    }

                    // applyCount: false — the whole period window; the count
                    // limit is applied to the combined live edge below.
                    let instances = try segmentInstances(mpd: mpd, template: template, options: options, applyCount: false)
                    for instance in instances {
                        let path = fillTemplate(media, representation: representation, number: instance.number, time: instance.time, subNumber: instance.subNumber)
                        guard let url = URL(string: path, relativeTo: base)?.absoluteURL else { continue }
                        let sortNumber = instance.subNumber.map { instance.number * 1000 + $0 } ?? instance.number
                        mediaByKey[key, default: []].append(SegmentURL(
                            periodID: period.id,
                            adaptationType: type,
                            representationID: representation.id,
                            isInitialization: false,
                            number: sortNumber,
                            time: instance.time,
                            duration: instance.duration,
                            url: url,
                            groupKey: instance.subNumber != nil ? instance.number : nil
                        ))
                    }
                }
            }
        }

        if !sawRepresentationID, let id = options.representationID {
            throw DashError.usage("No representation found with id '\(id)'.")
        }

        var output: [SegmentURL] = []
        for key in order {
            // The same init URL repeats in every period; keep each distinct one
            // (a mid-stream codec switch would change it) but not the dupes.
            var seenInit = Set<URL>()
            for seg in initByKey[key] ?? [] where seenInit.insert(seg.url).inserted {
                output.append(seg)
            }
            var media = mediaByKey[key] ?? []
            if let count = options.count, media.count > count {
                media = Array(media.suffix(count))
            }
            output.append(contentsOf: media)
        }

        guard !output.isEmpty else {
            throw DashError.unsupported("No segments matched this MPD and filter set.")
        }
        return output
    }

    static func mergedTemplate(_ parent: SegmentTemplate?, _ child: SegmentTemplate?) -> SegmentTemplate? {
        guard let parent else { return child }
        guard let child else { return parent }
        return SegmentTemplate(
            media: child.media ?? parent.media,
            initialization: child.initialization ?? parent.initialization,
            duration: child.duration ?? parent.duration,
            timescale: child.timescale ?? parent.timescale,
            startNumber: child.startNumber ?? parent.startNumber,
            presentationTimeOffset: child.presentationTimeOffset ?? parent.presentationTimeOffset,
            timeline: child.timeline.isEmpty ? parent.timeline : child.timeline
        )
    }

    static func representationBaseURL(mpd: MPD,
                                      period: Period,
                                      adaptation: AdaptationSet,
                                      representation: Representation,
                                      override: URL?) -> URL {
        if let override {
            return directoryURLKeepingPath(for: override)
        }

        var base = directoryURL(for: mpd.sourceURL)
        for part in [mpd.baseURL, period.baseURL, adaptation.baseURL, representation.baseURL].compactMap({ $0 }) {
            if let next = URL(string: part, relativeTo: base) {
                base = directoryURLKeepingPath(for: next.absoluteURL)
            }
        }
        return base
    }

    /// - Parameter applyCount: when false, the SegmentTimeline branch returns
    ///   the period's whole in-manifest window instead of just the first
    ///   `count`. The caller aggregates several periods and applies the count
    ///   limit to the combined live edge itself — limiting here (per period)
    ///   would drop the tail of every period longer than `count`. See
    ///   `expandSegments`.
    static func segmentInstances(mpd: MPD, template: SegmentTemplate, options: Options, applyCount: Bool = true) throws -> [SegmentInstance] {
        let startNumber = options.fromNumber ?? template.startNumber ?? 1

        if !template.timeline.isEmpty {
            let allInstances = expandTimeline(template: template, startNumber: template.startNumber ?? 1)
            let filtered = allInstances.filter { $0.number >= startNumber }
            return applyCount ? limit(instances: filtered, count: options.count) : filtered
        }

        guard let rawDuration = template.duration, rawDuration > 0 else {
            throw DashError.unsupported("SegmentTemplate needs either SegmentTimeline or duration.")
        }

        let timescale = template.timescale ?? 1
        let segmentDuration = rawDuration / timescale
        let count = options.count

        if let from = options.fromNumber {
            return (from..<(from + (count ?? 10))).map { SegmentInstance(number: $0, time: nil, duration: segmentDuration, subNumber: nil) }
        }

        if mpd.type == "dynamic" {
            let ast = mpd.availabilityStartTime ?? Date(timeIntervalSince1970: 0)
            let elapsed = max(0, Date().timeIntervalSince(ast))
            let start = template.startNumber ?? 1
            let latest = max(start, Int(floor(elapsed / segmentDuration)) + start - 1)
            let windowSeconds = mpd.timeShiftBufferDepth ?? (segmentDuration * Double(count ?? 10))
            let windowCount = max(1, Int(ceil(windowSeconds / segmentDuration)))
            let wanted = count ?? min(windowCount, 50)
            let first = max(start, latest - wanted + 1)
            return (first...latest).map { SegmentInstance(number: $0, time: nil, duration: segmentDuration, subNumber: nil) }
        }

        if let duration = mpd.mediaPresentationDuration {
            let total = max(1, Int(ceil(duration / segmentDuration)))
            let end = startNumber + total - 1
            return limit(instances: (startNumber...end).map { SegmentInstance(number: $0, time: nil, duration: segmentDuration, subNumber: nil) }, count: count)
        }

        return (startNumber..<(startNumber + (count ?? 10))).map { SegmentInstance(number: $0, time: nil, duration: segmentDuration, subNumber: nil) }
    }

    static func expandTimeline(template: SegmentTemplate, startNumber: Int) -> [SegmentInstance] {
        var instances: [SegmentInstance] = []
        var number = startNumber
        var currentTime: Int64 = template.presentationTimeOffset ?? 0
        // DASH-IF low-latency chunked assets (`chunkdurssr`) address each
        // segment as `k` separately fetchable CMAF chunks via $SubNumber$
        // instead of one whole segment — only split when the media template
        // actually references it, so ordinary streams that happen to carry
        // an (unrelated) `k` attribute aren't affected.
        let usesSubNumber = template.media?.contains("$SubNumber$") ?? false

        for entry in template.timeline {
            if let explicitTime = entry.t {
                currentTime = explicitTime
            }
            let repetitions = entry.r < 0 ? 0 : entry.r
            for _ in 0...repetitions {
                if usesSubNumber, let k = entry.k, k > 0 {
                    let baseChunkDuration = entry.d / Int64(k)
                    let remainder = entry.d % Int64(k)
                    var chunkTime = currentTime
                    for sub in 1...k {
                        let chunkDuration = baseChunkDuration + (sub == k ? remainder : 0)
                        instances.append(SegmentInstance(
                            number: number,
                            time: chunkTime,
                            duration: Double(chunkDuration) / (template.timescale ?? 1),
                            subNumber: sub
                        ))
                        chunkTime += chunkDuration
                    }
                } else {
                    instances.append(SegmentInstance(number: number, time: currentTime, duration: Double(entry.d) / (template.timescale ?? 1), subNumber: nil))
                }
                number += 1
                currentTime += entry.d
            }
        }
        return instances
    }

    static func printInfo(mpd: MPD, options: Options) {
        do {
            let periods = try selectedPeriods(mpd: mpd, options: options)
            try ensureRepresentationExists(in: periods, options: options)
            if options.json {
                printJSON(infoJSON(mpd: mpd, periods: periods, options: options))
                return
            }

            print("MPD: \(mpd.sourceURL.absoluteString)")
            print("type: \(mpd.type ?? "static")")
            if let duration = mpd.mediaPresentationDuration { print("duration: \(duration)s") }
            if let update = mpd.minimumUpdatePeriod { print("minimumUpdatePeriod: \(update)s") }
            if let depth = mpd.timeShiftBufferDepth { print("timeShiftBufferDepth: \(depth)s") }

            for (periodIndex, period) in periods.enumerated() {
                print("")
                print("period[\(periodIndex)] id=\(period.id ?? "-") start=\(period.start ?? "-")")
                for adaptation in period.adaptationSets {
                    guard passesTypeFilters(adaptation: adaptation, options: options) else {
                        continue
                    }
                    let representations = matchingRepresentations(adaptation: adaptation, options: options)
                    if options.representationID != nil && representations.isEmpty {
                        continue
                    }
                    let type = adaptationType(adaptation)
                    print("  adaptation type=\(type) id=\(adaptation.id ?? "-") lang=\(adaptation.lang ?? "-") mime=\(adaptation.mimeType ?? "-")")
                    for representation in representations {
                        print("    representation id=\(representation.id) bandwidth=\(representation.bandwidth ?? "-") codecs=\(representation.codecs ?? adaptation.codecs ?? "-") width=\(representation.width ?? "-") height=\(representation.height ?? "-") frameRate=\(representation.frameRate ?? "-") audioSamplingRate=\(representation.audioSamplingRate ?? "-")")
                    }
                }
            }
        } catch {
            fputs("error: \(error)\n", stderr)
            exit(1)
        }
    }

    static func infoJSON(mpd: MPD, periods: [Period], options: Options) -> [String: Any] {
        [
            "mpd": mpd.sourceURL.absoluteString,
            "type": mpd.type ?? "static",
            "mediaPresentationDuration": mpd.mediaPresentationDuration as Any,
            "minimumUpdatePeriod": mpd.minimumUpdatePeriod as Any,
            "timeShiftBufferDepth": mpd.timeShiftBufferDepth as Any,
            "periods": periods.enumerated().map { index, period in
                [
                    "index": index,
                    "id": period.id as Any,
                    "start": period.start as Any,
                    "adaptationSets": period.adaptationSets.compactMap { adaptation -> [String: Any]? in
                        let type = adaptationType(adaptation)
                        guard passesTypeFilters(adaptation: adaptation, options: options) else { return nil }
                        let representations = matchingRepresentations(adaptation: adaptation, options: options)
                        if options.representationID != nil && representations.isEmpty { return nil }
                        return [
                            "id": adaptation.id as Any,
                            "type": type,
                            "mimeType": adaptation.mimeType as Any,
                            "language": adaptation.lang as Any,
                            "codecs": adaptation.codecs as Any,
                            "representations": representations.map { representationJSON(adaptation: adaptation, representation: $0) }
                        ]
                    }
                ]
            }
        ]
    }

    /// Fetches and parses an MPD, lists every representation (video+audio,
    /// across all periods), and — for video representations — fetches each
    /// one's init segment to detect CENC KIDs via CENCDecryptor.detectProtection.
    /// This is the single source of truth for the panel's "Detect" button in
    /// the stream editor; PanelServer only calls this, it doesn't parse MPDs
    /// itself.
    static func probe(mpdURL: URL, headers: [(String, String)], proxy: String?, timeout: TimeInterval) throws -> [String: Any] {
        let client = HTTPClient(options: HTTPRequestOptions(timeout: timeout, headers: headers, proxy: proxy))
        let data = try client.get(mpdURL)
        let mpd = try MPDParser(sourceURL: mpdURL).parse(data: data)

        var representations: [[String: Any]] = []
        var kidsByRepresentation: [String: [String]] = [:]

        for period in mpd.periods {
            for adaptation in period.adaptationSets {
                for representation in adaptation.representations {
                    representations.append(representationJSON(adaptation: adaptation, representation: representation))
                    guard adaptationType(adaptation) == "video" else { continue }
                    var initOptions = Options(mode: .list)
                    initOptions.mpdURL = mpdURL
                    initOptions.count = 1
                    initOptions.includeInitialization = true
                    initOptions.headers = headers
                    initOptions.proxy = proxy
                    initOptions.timeout = timeout
                    initOptions.representationID = representation.id
                    if let segments = try? expandSegments(mpd: mpd, options: initOptions),
                       let initSegment = segments.first(where: { $0.isInitialization }),
                       let initData = try? client.get(initSegment.url) {
                        let kids = CENCDecryptor.detectProtection(initData)
                        if !kids.isEmpty { kidsByRepresentation[representation.id] = kids }
                    }
                }
            }
        }

        return [
            "kind": "mpd",
            "representations": representations,
            "protection": kidsByRepresentation
        ]
    }

    static func representationJSON(adaptation: AdaptationSet, representation: Representation) -> [String: Any] {
        [
            "id": representation.id,
            "type": adaptationType(adaptation),
            "language": adaptation.lang as Any,
            "bandwidth": int(representation.bandwidth) as Any,
            "bandwidthRaw": representation.bandwidth as Any,
            "codecs": optionalAny(representation.codecs ?? adaptation.codecs),
            "width": int(representation.width) as Any,
            "height": int(representation.height) as Any,
            "frameRate": representation.frameRate as Any,
            "audioSamplingRate": int(representation.audioSamplingRate) as Any,
            "mimeType": adaptation.mimeType as Any
        ]
    }

    static func segmentJSON(_ segment: SegmentURL) -> [String: Any] {
        [
            "periodId": optionalAny(segment.periodID),
            "type": segment.adaptationType,
            "representationId": segment.representationID,
            "isInitialization": segment.isInitialization,
            "number": optionalAny(segment.number),
            "time": optionalAny(segment.time),
            "duration": optionalAny(segment.duration),
            "url": segment.url.absoluteString
        ]
    }

    static func optionalAny<T>(_ value: T?) -> Any {
        value as Any
    }

    static func printJSON(_ object: Any) {
        let normalized = normalizeJSON(object)
        if let data = try? JSONSerialization.data(withJSONObject: normalized, options: [.prettyPrinted, .sortedKeys]),
           let text = String(data: data, encoding: .utf8) {
            print(text)
        }
    }

    static func normalizeJSON(_ value: Any) -> Any {
        if let optional = Mirror(reflecting: value).children.first, Mirror(reflecting: value).displayStyle == .optional {
            return normalizeJSON(optional.value)
        }
        if Mirror(reflecting: value).displayStyle == .optional {
            return NSNull()
        }
        if let dictionary = value as? [String: Any] {
            return dictionary.mapValues(normalizeJSON)
        }
        if let array = value as? [Any] {
            return array.map(normalizeJSON)
        }
        return value
    }

    static func limit(instances: [SegmentInstance], count: Int?) -> [SegmentInstance] {
        guard let count else { return instances }
        return Array(instances.prefix(count))
    }

    static func fillTemplate(_ template: String,
                             representation: Representation,
                             number: Int?,
                             time: Int64?,
                             subNumber: Int? = nil) -> String {
        var value = template
        value = replaceToken("RepresentationID", in: value, with: representation.id)
        value = replaceToken("Bandwidth", in: value, with: representation.bandwidth ?? "")
        if let number {
            value = replaceToken("Number", in: value, with: number)
        }
        if let time {
            value = replaceToken("Time", in: value, with: time)
        }
        if let subNumber {
            value = replaceToken("SubNumber", in: value, with: subNumber)
        }
        return value
    }

    // The padded-width token patterns (`$Number%05d$` etc.) are compiled once
    // per distinct token and reused. fillTemplate calls replaceToken five times
    // for every segment it generates, so on a live stream this used to compile
    // the same five regexes for every segment on every poll.
    private static let tokenRegexCacheLock = NSLock()
    private static var tokenRegexCache: [String: NSRegularExpression] = [:]

    private static func paddedTokenRegex(_ token: String) -> NSRegularExpression? {
        tokenRegexCacheLock.lock(); defer { tokenRegexCacheLock.unlock() }
        if let cached = tokenRegexCache[token] { return cached }
        guard let regex = try? NSRegularExpression(pattern: "\\$\(token)%0([0-9]+)d\\$") else { return nil }
        tokenRegexCache[token] = regex
        return regex
    }

    static func replaceToken<T>(_ token: String, in template: String, with value: T) -> String {
        var output = template.replacingOccurrences(of: "$\(token)$", with: "\(value)")
        if let regex = paddedTokenRegex(token) {
            let range = NSRange(output.startIndex..<output.endIndex, in: output)
            let matches = regex.matches(in: output, range: range).reversed()
            for match in matches {
                guard match.numberOfRanges == 2,
                      let fullRange = Range(match.range(at: 0), in: output),
                      let widthRange = Range(match.range(at: 1), in: output),
                      let width = Int(output[widthRange]) else { continue }
                let replacement: String
                if let intValue = value as? Int {
                    replacement = String(format: "%0\(width)d", intValue)
                } else if let int64Value = value as? Int64 {
                    replacement = String(format: "%0\(width)lld", int64Value)
                } else {
                    replacement = "\(value)"
                }
                output.replaceSubrange(fullRange, with: replacement)
            }
        }
        return output
    }

    static func directoryURL(for url: URL) -> URL {
        let directory = url.hasDirectoryPath ? url : url.deletingLastPathComponent()
        let text = directory.absoluteString
        if text.hasSuffix("/") {
            return directory
        }
        return URL(string: text + "/") ?? directory
    }

    static func directoryURLKeepingPath(for url: URL) -> URL {
        let text = url.absoluteString
        if text.hasSuffix("/") {
            return url
        }
        return URL(string: text + "/") ?? url
    }

    static func usage() -> String {
        """
        Usage:
          ./dashsegments --list [options] MPD_URL
          ./dashsegments --info [options] MPD_URL
          ./dashsegments action=FUNCTION key=value ...

        Actions:
          --list                    Print segment URLs. Initialization URLs are included by default.
          --info                    Print MPD periods, adaptation sets, representations, codecs, sizes, languages, and bitrates.
          action=describeFunctions  Print JSON docs for external-script actions.
          -h, --help                Show this help.

        Filtering:
          --representation ID       Only show one Representation@id.
          --video-only              Only video adaptation sets.
          --audio-only              Only audio adaptation sets.
          --period ID_OR_INDEX      Select a period by id or zero-based index.

        Segment options:
          --count N                 Print N media segments per representation.
          --from N                  Start at DASH segment number N.
          --no-init                 Do not print initialization segment URLs.
          --base-url URL            Override the base URL used to resolve segment paths.

        Request options:
          -H, --header 'K: V'       Add an HTTP request header. Can be repeated.
          --proxy URL               Use an HTTP or HTTPS proxy, for example http://127.0.0.1:8888.
          --timeout SECONDS         Request timeout. Default: 30.
          --sleep-requests SECONDS  For live MPDs, keep polling and print only new segment URLs.

        Output:
          --json                    Print JSON instead of plain text.

        Examples:
          ./dashsegments --list --count 5 https://livesim2.dashif.org/livesim2/testpic_2s/Manifest.mpd
          ./dashsegments --info --video-only https://livesim2.dashif.org/livesim2/testpic_2s/Manifest.mpd
          ./dashsegments --list --representation V300 --sleep-requests 2 https://livesim2.dashif.org/livesim2/testpic_2s/Manifest.mpd
          ./dashsegments action=listSegments mpd=https://example.com/manifest.mpd representation=2 count=5
        """
    }
}

func int(_ value: String?) -> Int? {
    guard let value else { return nil }
    return Int(value)
}

func int64(_ value: String?) -> Int64? {
    guard let value else { return nil }
    return Int64(value)
}

func double(_ value: String?) -> Double? {
    guard let value else { return nil }
    return Double(value)
}

func parseDuration(_ text: String?) -> Double? {
    guard let text, text.hasPrefix("P") else { return nil }

    var total = 0.0
    var number = ""
    var inTime = false
    let body = text[text.index(after: text.startIndex)...]

    for char in body {
        if char == "T" {
            inTime = true
            continue
        }
        if char.isNumber || char == "." {
            number.append(char)
            continue
        }
        guard let value = Double(number) else {
            number = ""
            continue
        }

        switch char {
        case "D":
            total += value * 86_400
        case "H":
            total += value * 3_600
        case "M":
            total += value * (inTime ? 60 : 2_592_000)
        case "S":
            total += value
        default:
            break
        }
        number = ""
    }

    return total
}
