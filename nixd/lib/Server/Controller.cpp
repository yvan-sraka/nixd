#include "nixd-config.h"

#include "nixd/AST/ParseAST.h"
#include "nixd/Expr/Expr.h"
#include "nixd/Parser/Parser.h"
#include "nixd/Parser/Require.h"
#include "nixd/Server/ASTManager.h"
#include "nixd/Server/EvalDraftStore.h"
#include "nixd/Server/Server.h"
#include "nixd/Support/Diagnostic.h"
#include "nixd/Support/Support.h"

#include "lspserver/Connection.h"
#include "lspserver/Logger.h"
#include "lspserver/Path.h"
#include "lspserver/Protocol.h"
#include "lspserver/SourceCode.h"
#include "lspserver/URI.h"

#include <llvm/ADT/FunctionExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/ScopedPrinter.h>
#include <llvm/Support/raw_ostream.h>

#include <boost/iostreams/stream.hpp>
#include <boost/process.hpp>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <unistd.h>

namespace nixd {

void Server::forkWorker(llvm::unique_function<void()> WorkerAction,
                        std::deque<std::unique_ptr<Proc>> &WorkerPool,
                        size_t Size) {
  if (Role != ServerRole::Controller)
    return;
  auto To = std::make_unique<nix::Pipe>();
  auto From = std::make_unique<nix::Pipe>();

  To->create();
  From->create();

  auto ForkPID = fork();
  if (ForkPID == -1) {
    lspserver::elog("Cannot create child worker process");
    // TODO reason?
  } else if (ForkPID == 0) {
    // child, it should be a COW fork of the parent process info, as a snapshot
    // we will leave the evaluation task (or any language feature) to the child
    // process, and forward language feature requests to this childs, and choose
    // the best response.
    auto ChildPID = getpid();
    lspserver::elog("created child worker process {0}", ChildPID);

    // Redirect stdin & stdout to our pipes, instead of LSP clients
    dup2(To->readSide.get(), 0);
    dup2(From->writeSide.get(), 1);

    WorkerAction();

    // Communicate the controller in stadnard mode, instead of lit testing
    switchStreamStyle(lspserver::JSONStreamStyle::Standard);

  } else {

    auto WorkerInputDispatcher =
        std::thread([In = From->readSide.get(), this]() {
          // Start a thread that handles outputs from WorkerProc, and call the
          // Controller Callback
          auto IPort = std::make_unique<lspserver::InboundPort>(In);

          // The loop will exit when the worker process close the pipe.
          IPort->loop(*this);
        });

    auto ProcFdStream =
        std::make_unique<llvm::raw_fd_ostream>(To->writeSide.get(), false);

    auto OutPort =
        std::make_unique<lspserver::OutboundPort>(*ProcFdStream, false);

    auto WorkerProc = std::unique_ptr<Proc>(
        new Proc{.ToPipe = std::move(To),
                 .FromPipe = std::move(From),
                 .OutPort = std::move(OutPort),
                 .OwnedStream = std::move(ProcFdStream),
                 .Pid = ForkPID,
                 .WorkspaceVersion = WorkspaceVersion,
                 .InputDispatcher = std::move(WorkerInputDispatcher),
                 .Smp = std::ref(FinishSmp),
                 .WaitWorker = WaitWorker});

    WorkerPool.emplace_back(std::move(WorkerProc));
    if (WorkerPool.size() > Size && !WaitWorker)
      WorkerPool.pop_front();
  }
}

void Server::updateWorkspaceVersion() {
  if (Role != ServerRole::Controller)
    return;
  WorkspaceVersion++;
  std::lock_guard EvalGuard(EvalWorkerLock);
  // The eval worker
  forkWorker([this]() { switchToEvaluator(); }, EvalWorkers,
             Config.eval.workers);
}

void Server::addDocument(lspserver::PathRef File, llvm::StringRef Contents,
                         llvm::StringRef Version) {
  using namespace lspserver;
  auto IVersion = DraftStore::decodeVersion(Version);
  // Since this file is updated, we first clear its diagnostic
  PublishDiagnosticsParams Notification;
  Notification.uri = URIForFile::canonicalize(File, File);
  Notification.diagnostics = {};
  Notification.version = IVersion;
  PublishDiagnostic(Notification);

  DraftMgr.addDraft(File, Version, Contents);
  ASTMgr.schedParse(Contents.str(), File.str(), IVersion.value_or(0));
  updateWorkspaceVersion();
}

void Server::updateConfig(configuration::TopLevel &&NewConfig) {
  Config = std::move(NewConfig);
  forkOptionWorker();
  updateWorkspaceVersion();
}

void Server::fetchConfig() {
  if (ClientCaps.WorkspaceConfiguration) {
    WorkspaceConfiguration(
        lspserver::ConfigurationParams{
            std::vector<lspserver::ConfigurationItem>{
                lspserver::ConfigurationItem{.section = "nixd"}}},
        [this](llvm::Expected<configuration::TopLevel> Response) {
          if (Response) {
            updateConfig(std::move(Response.get()));
          }
        });
  }
}

llvm::Expected<configuration::TopLevel>
Server::parseConfig(llvm::StringRef JSON) {
  using namespace configuration;

  auto ExpectedValue = llvm::json::parse(JSON);
  if (!ExpectedValue)
    return ExpectedValue.takeError();
  TopLevel NewConfig;
  llvm::json::Path::Root P;
  if (fromJSON(ExpectedValue.get(), NewConfig, P))
    return NewConfig;
  return lspserver::error("value cannot be converted to internal config type");
}

void Server::readJSONConfig(lspserver::PathRef File) noexcept {
  try {
    std::string ConfigStr;
    std::ostringstream SS;
    std::ifstream In(File.str(), std::ios::in);
    SS << In.rdbuf();

    if (auto NewConfig = parseConfig(SS.str()))
      updateConfig(std::move(NewConfig.get()));
    else {
      throw nix::Error("configuration cannot be parsed");
    }
  } catch (std::exception &E) {
  } catch (...) {
  }
}

std::string Server::encodeVersion(std::optional<int64_t> LSPVersion) {
  return LSPVersion ? llvm::to_string(*LSPVersion) : "";
}

std::shared_ptr<const std::string>
Server::getDraft(lspserver::PathRef File) const {
  auto Draft = DraftMgr.getDraft(File);
  if (!Draft)
    return nullptr;
  return std::move(Draft->Contents);
}

Server::Server(std::unique_ptr<lspserver::InboundPort> In,
               std::unique_ptr<lspserver::OutboundPort> Out, int WaitWorker)
    : LSPServer(std::move(In), std::move(Out)), WaitWorker(WaitWorker),
      ASTMgr(Pool) {

  // Life Cycle
  Registry.addMethod("initialize", this, &Server::onInitialize);
  Registry.addNotification("initialized", this, &Server::onInitialized);

  // Text Document Synchronization
  Registry.addNotification("textDocument/didOpen", this,
                           &Server::onDocumentDidOpen);
  Registry.addNotification("textDocument/didChange", this,
                           &Server::onDocumentDidChange);

  Registry.addNotification("textDocument/didClose", this,
                           &Server::onDocumentDidClose);

  // Language Features
  Registry.addMethod("textDocument/documentLink", this,
                     &Server::onDocumentLink);
  Registry.addMethod("textDocument/documentSymbol", this,
                     &Server::onDocumentSymbol);
  Registry.addMethod("textDocument/hover", this, &Server::onHover);
  Registry.addMethod("textDocument/completion", this, &Server::onCompletion);
  Registry.addMethod("textDocument/declaration", this, &Server::onDecalration);
  Registry.addMethod("textDocument/definition", this, &Server::onDefinition);
  Registry.addMethod("textDocument/formatting", this, &Server::onFormat);
  Registry.addMethod("textDocument/rename", this, &Server::onRename);
  Registry.addMethod("textDocument/prepareRename", this,
                     &Server::onPrepareRename);

  PublishDiagnostic = mkOutNotifiction<lspserver::PublishDiagnosticsParams>(
      "textDocument/publishDiagnostics");

  // Workspace
  Registry.addNotification("workspace/didChangeConfiguration", this,
                           &Server::onWorkspaceDidChangeConfiguration);
  WorkspaceConfiguration =
      mkOutMethod<lspserver::ConfigurationParams, configuration::TopLevel>(
          "workspace/configuration");

  /// IPC
  Registry.addNotification("nixd/ipc/diagnostic", this,
                           &Server::onEvalDiagnostic);

  Registry.addMethod("nixd/ipc/textDocument/hover", this, &Server::onEvalHover);

  Registry.addMethod("nixd/ipc/option/textDocument/declaration", this,
                     &Server::onOptionDeclaration);

  Registry.addNotification("nixd/ipc/finished", this, &Server::onFinished);

  readJSONConfig();
}

//-----------------------------------------------------------------------------/
// Life Cycle

void Server::onInitialize(const lspserver::InitializeParams &InitializeParams,
                          lspserver::Callback<llvm::json::Value> Reply) {
  ClientCaps = InitializeParams.capabilities;
  llvm::json::Object ServerCaps{
      {"textDocumentSync",
       llvm::json::Object{
           {"openClose", true},
           {"change", (int)lspserver::TextDocumentSyncKind::Incremental},
           {"save", true},
       }},
      {"declarationProvider", true},
      {"definitionProvider", true},
      {"documentLinkProvider", llvm::json::Object{{"resolveProvider", false}}},
      {"documentSymbolProvider", true},
      {"hoverProvider", true},
      {"documentFormattingProvider", true},
      {
          "completionProvider",
          llvm::json::Object{{"triggerCharacters", {"."}}},
      },
      {"renameProvider", llvm::json::Object{{"prepareProvider", true}}}};

  llvm::json::Object Result{
      {{"serverInfo",
        llvm::json::Object{{"name", "nixd"}, {"version", NIXD_VERSION}}},
       {"capabilities", std::move(ServerCaps)}}};
  Reply(std::move(Result));
}

//-----------------------------------------------------------------------------/
// Text Document Synchronization

void Server::onDocumentDidOpen(
    const lspserver::DidOpenTextDocumentParams &Params) {
  lspserver::PathRef File = Params.textDocument.uri.file();

  const std::string &Contents = Params.textDocument.text;

  addDocument(File, Contents, encodeVersion(Params.textDocument.version));
}

void Server::onDocumentDidChange(
    const lspserver::DidChangeTextDocumentParams &Params) {
  lspserver::PathRef File = Params.textDocument.uri.file();
  auto Code = getDraft(File);
  if (!Code) {
    lspserver::log("Trying to incrementally change non-added document: {0}",
                   File);
    return;
  }
  std::string NewCode(*Code);
  for (const auto &Change : Params.contentChanges) {
    if (auto Err = applyChange(NewCode, Change)) {
      // If this fails, we are most likely going to be not in sync anymore
      // with the client.  It is better to remove the draft and let further
      // operations fail rather than giving wrong results.
      removeDocument(File);
      lspserver::elog("Failed to update {0}: {1}", File, std::move(Err));
      return;
    }
  }
  addDocument(File, NewCode, encodeVersion(Params.textDocument.version));
}

void Server::onDocumentDidClose(
    const lspserver::DidCloseTextDocumentParams &Params) {
  lspserver::PathRef File = Params.textDocument.uri.file();
  auto Code = getDraft(File);
  removeDocument(File);
}

//-----------------------------------------------------------------------------/
// Language Features

void Server::onDecalration(const lspserver::TextDocumentPositionParams &Params,
                           lspserver::Callback<llvm::json::Value> Reply) {
  if (!Config.options.enable) {
    Reply(nullptr);
    return;
  }

  auto Task = [=, Reply = std::move(Reply), this]() mutable {
    ReplyRAII<llvm::json::Value> RR(std::move(Reply));

    // Set the default response to "null", instead of errors
    RR.Response = nullptr;

    ipc::AttrPathParams APParams;

    // Try to get current attribute path, expand the position

    auto Code = llvm::StringRef(
        *DraftMgr.getDraft(Params.textDocument.uri.file())->Contents);
    auto ExpectedOffset = positionToOffset(Code, Params.position);

    if (!ExpectedOffset)
      RR.Response = ExpectedOffset.takeError();

    auto Offset = ExpectedOffset.get();
    auto From = Offset;
    auto To = Offset;

    std::string Punc = "\r\n\t ;";

    auto IsPunc = [&Punc](char C) {
      for (const auto Char : Punc) {
        if (Char == C)
          return true;
      }
      return false;
    };

    for (; From >= 0 && !IsPunc(Code[From]);)
      From--;
    for (; To < Code.size() && !IsPunc(Code[To]);)
      To++;

    APParams.Path = Code.substr(From, To - From).trim(Punc);
    lspserver::log("requesting path: {0}", APParams.Path);

    auto Responses = askWC<lspserver::Location>(
        "nixd/ipc/option/textDocument/declaration", APParams,
        {OptionWorkers, OptionWorkerLock, 2e4});

    if (Responses.empty())
      return;

    RR.Response = std::move(Responses.back());
  };

  boost::asio::post(Pool, std::move(Task));
}

void Server::onDefinition(const lspserver::TextDocumentPositionParams &Params,
                          lspserver::Callback<llvm::json::Value> Reply) {
  using RTy = lspserver::Location;
  using namespace lspserver;
  using V = llvm::json::Value;
  using O = llvm::json::Object;

  auto Task = [=, Reply = std::move(Reply), this]() mutable {
    // Firstly, ask the eval workers if there is a suitable location
    // Prefer evaluated locations because in most case this is more useful
    constexpr auto Method = "nixd/ipc/textDocument/definition";
    auto Resp =
        askWC<RTy>(Method, Params, WC{EvalWorkers, EvalWorkerLock, 1e6});
    if (!Resp.empty()) {
      Reply(latestMatchOr<RTy, V>(
          Resp, [](const RTy &Location) -> bool { return true; }, O{}));
      return;
    }

    // Otherwise, statically find the definition
    const auto &URI = Params.textDocument.uri;
    auto Path = URI.file().str();
    const auto &Pos = Params.position;

    auto Action = [=](ReplyRAII<llvm::json::Value> &&RR, const ParseAST &AST,
                      ASTManager::VersionTy Version) {
      try {
        Location L;
        auto Def = AST.def(Pos);
        L.range = AST.defRange(Def);
        L.uri = URI;
        RR.Response = std::move(L);
      } catch (std::exception &E) {
        // Reply an error is annoying at the user interface, let's just log.
        auto ErrMsg = stripANSI(E.what());
        RR.Response = O{};
        elog("static definition: {0}", std::move(ErrMsg));
      }
    };

    withParseAST<V>({std::move(Reply)}, Path, std::move(Action));
  };

  boost::asio::post(Pool, std::move(Task));
}

void Server::onDocumentLink(
    const lspserver::DocumentLinkParams &Params,
    lspserver::Callback<std::vector<lspserver::DocumentLink>> Reply) {
  auto Task = [=, Reply = std::move(Reply), this]() mutable {
    auto Path = Params.textDocument.uri.file().str();
    auto Action = [File = Path](ReplyRAII<ParseAST::Links> &&RR,
                                const ParseAST &AST,
                                ASTManager::VersionTy Version) {
      RR.Response = AST.documentLink(File);
    };
    auto RR = ReplyRAII<ParseAST::Links>(std::move(Reply));
    withParseAST<ParseAST::Links>(std::move(RR), Path, std::move(Action));
  };

  boost::asio::post(Pool, std::move(Task));
}

void Server::onDocumentSymbol(
    const lspserver::DocumentSymbolParams &Params,
    lspserver::Callback<std::vector<lspserver::DocumentSymbol>> Reply) {

  auto Task = [=, Reply = std::move(Reply), this]() mutable {
    auto Action = [](ReplyRAII<ParseAST::Symbols> &&RR, const ParseAST &AST,
                     ASTManager::VersionTy Version) {
      RR.Response = AST.documentSymbol();
    };
    auto RR = ReplyRAII<ParseAST::Symbols>(std::move(Reply));
    auto Path = Params.textDocument.uri.file().str();
    withParseAST<ParseAST::Symbols>(std::move(RR), Path, std::move(Action));
  };

  boost::asio::post(Pool, std::move(Task));
}

void Server::onHover(const lspserver::TextDocumentPositionParams &Params,
                     lspserver::Callback<lspserver::Hover> Reply) {
  using RTy = lspserver::Hover;
  constexpr auto Method = "nixd/ipc/textDocument/hover";
  auto Task = [=, Reply = std::move(Reply), this]() mutable {
    auto Resp =
        askWC<RTy>(Method, Params, WC{EvalWorkers, EvalWorkerLock, 2e6});
    Reply(latestMatchOr<lspserver::Hover>(Resp, [](const lspserver::Hover &H) {
      return H.contents.value.length() != 0;
    }));
  };

  boost::asio::post(Pool, std::move(Task));
}

void Server::onCompletion(const lspserver::CompletionParams &Params,
                          lspserver::Callback<llvm::json::Value> Reply) {
  // Statically construct the completion list.
  std::binary_semaphore Smp(0);
  auto Path = Params.textDocument.uri.file();
  auto Action = [&Smp, &Params, &Reply,
                 this](const ParseAST &AST,
                       ASTManager::VersionTy Version) mutable {
    using PL = ParseAST::LocationContext;
    using List = lspserver::CompletionList;
    ReplyRAII<llvm::json::Value> RR(std::move(Reply));

    // TODO: split this in AST, use AST-based attrpath construction.
    auto CompletionFromOptions = [&, this]() -> std::optional<List> {
      // Requested completion on an attribute name, not values.
      if (Config.options.enable) {
        ipc::AttrPathParams APParams;

        if (Params.context.triggerCharacter == ".") {
          // Get nixpkgs options
          auto Code = llvm::StringRef(
              *DraftMgr.getDraft(Params.textDocument.uri.file())->Contents);
          auto ExpectedPosition = positionToOffset(Code, Params.position);

          // get the attr path
          auto TruncateBackCode = Code.substr(0, ExpectedPosition.get());

          auto [_, AttrPath] = TruncateBackCode.rsplit(" ");
          APParams.Path = AttrPath.str();
        }

        auto Resp = askWC<lspserver::CompletionList>(
            "nixd/ipc/textDocument/completion/options", APParams,
            {OptionWorkers, OptionWorkerLock, 1e5});
        if (!Resp.empty())
          return Resp.back();
      }
      return std::nullopt;
    };

    auto CompletionFromEval = [&, this]() -> std::optional<List> {
      constexpr auto Method = "nixd/ipc/textDocument/completion";
      auto Resp =
          askWC<List>(Method, Params, {EvalWorkers, EvalWorkerLock, 2e6});
      std::optional<List> R;
      if (!Resp.empty())
        return std::move(Resp.back());
      return std::nullopt;
    };

    switch (AST.getContext(Params.position)) {
    case (PL::AttrName): {
      RR.Response = CompletionFromOptions();
      break;
    }
    case (PL::Value): {
      RR.Response = CompletionFromEval();
      break;
    }
    case (PL::Unknown):
      List L;
      if (auto LOptions = CompletionFromOptions())
        L.items.insert(L.items.end(), (*LOptions).items.begin(),
                       (*LOptions).items.end());
      if (auto LEval = CompletionFromEval())
        L.items.insert(L.items.end(), (*LEval).items.begin(),
                       (*LEval).items.end());
      L.isIncomplete = true;
      RR.Response = std::move(L);
    }
    Smp.release();
  };

  if (auto Draft = DraftMgr.getDraft(Path)) {
    auto Version = EvalDraftStore::decodeVersion(Draft->Version).value_or(0);
    ASTMgr.withAST(Path.str(), Version, std::move(Action));
    Smp.acquire();
  } else {
    Reply(lspserver::error("requested completion list on unknown draft path"));
  }
}

void Server::onRename(const lspserver::RenameParams &Params,
                      lspserver::Callback<lspserver::WorkspaceEdit> Reply) {

  auto Task = [Params, Reply = std::move(Reply), this]() mutable {
    auto URI = Params.textDocument.uri;
    auto Path = URI.file().str();
    auto Action = [URI, Params](ReplyRAII<lspserver::WorkspaceEdit> &&RR,
                                const ParseAST &AST,
                                ASTManager::VersionTy Version) {
      std::map<std::string, std::vector<lspserver::TextEdit>> Changes;
      if (auto Edits = AST.rename(Params.position, Params.newName)) {
        Changes[URI.uri()] = std::move(*Edits);
        RR.Response = lspserver::WorkspaceEdit{.changes = Changes};
        return;
      }
      RR.Response = lspserver::error("no rename edits available");
      return;
    };
    withParseAST<lspserver::WorkspaceEdit>(
        ReplyRAII<lspserver::WorkspaceEdit>(std::move(Reply)), Path,
        std::move(Action));
  };

  boost::asio::post(Pool, std::move(Task));
}

void Server::onPrepareRename(
    const lspserver::TextDocumentPositionParams &Params,
    lspserver::Callback<llvm::json::Value> Reply) {
  auto Task = [Params, Reply = std::move(Reply), this]() mutable {
    auto Path = Params.textDocument.uri.file().str();
    auto Action = [Params](ReplyRAII<llvm::json::Value> &&RR,
                           const ParseAST &AST, ASTManager::VersionTy Version) {
      auto Pos = Params.position;
      if (auto Edits = AST.rename(Pos, "")) {
        for (const auto &Edit : *Edits) {
          if (Edit.range.contains(Pos)) {
            RR.Response = Edit.range;
            return;
          }
        }
      }
      RR.Response = lspserver::error("no rename edits available");
      return;
    };
    withParseAST<llvm::json::Value>(
        ReplyRAII<llvm::json::Value>(std::move(Reply)), Path,
        std::move(Action));
  };

  boost::asio::post(Pool, std::move(Task));
}

void Server::clearDiagnostic(lspserver::PathRef Path) {
  lspserver::URIForFile Uri = lspserver::URIForFile::canonicalize(Path, Path);
  clearDiagnostic(Uri);
}

void Server::clearDiagnostic(const lspserver::URIForFile &FileUri) {
  lspserver::PublishDiagnosticsParams Notification;
  Notification.uri = FileUri;
  Notification.diagnostics = {};
  PublishDiagnostic(Notification);
}

void Server::onEvalDiagnostic(const ipc::Diagnostics &Diag) {
  lspserver::log("received diagnostic from worker: {0}", Diag.WorkspaceVersion);

  {
    std::lock_guard<std::mutex> Guard(DiagStatusLock);
    if (DiagStatus.WorkspaceVersion > Diag.WorkspaceVersion) {
      // Skip this diagnostic, because it is outdated.
      return;
    }

    // Update client diagnostics
    DiagStatus.WorkspaceVersion = Diag.WorkspaceVersion;

    for (const auto &PublishedDiags : DiagStatus.ClientDiags)
      clearDiagnostic(PublishedDiags.uri);

    DiagStatus.ClientDiags = Diag.Params;

    for (const auto &Diag : Diag.Params) {
      PublishDiagnostic(Diag);
    }
  }
}

void Server::onFinished(const ipc::WorkerMessage &) { FinishSmp.release(); }

void Server::onFormat(
    const lspserver::DocumentFormattingParams &Params,
    lspserver::Callback<std::vector<lspserver::TextEdit>> Reply) {

  auto Task = [=, Reply = std::move(Reply), this]() mutable {
    lspserver::PathRef File = Params.textDocument.uri.file();
    auto Code = *getDraft(File);
    auto FormatFuture = std::async([Code = std::move(Code),
                                    this]() -> std::optional<std::string> {
      try {
        namespace bp = boost::process;
        bp::opstream To;
        bp::ipstream From;
        bp::child Fmt(Config.formatting.command, bp::std_out > From,
                      bp::std_in < To);

        To << Code;
        To.flush();

        To.pipe().close();

        std::string FormattedCode;
        while (From.good() && !From.eof()) {
          std::string Buf;
          std::getline(From, Buf, {});
          FormattedCode += Buf;
        }
        Fmt.wait();
        return FormattedCode;
      } catch (std::exception &E) {
        lspserver::elog(
            "cannot summon external formatting command, reason: {0}", E.what());
      } catch (...) {
      }
      return std::nullopt;
    });

    /// Wait for the external command, if this timeout, something went wrong
    auto Status = FormatFuture.wait_for(std::chrono::seconds(1));
    std::optional<std::string> FormattedCode;
    using lspserver::TextEdit;
    if (Status == std::future_status::ready)
      FormattedCode = FormatFuture.get();

    if (FormattedCode.has_value()) {
      TextEdit E{{{0, 0}, {INT_MAX, INT_MAX}}, FormattedCode.value()};
      Reply(std::vector{E});
    } else {
      Reply(lspserver::error("no formatting response received"));
    }
  };
  boost::asio::post(Pool, std::move(Task));
}
} // namespace nixd
