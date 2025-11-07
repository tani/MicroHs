{-# LANGUAGE ForeignFunctionInterface #-}
module Repl (
  mhsReplNew,
  mhsReplFree,
  mhsReplDefine,
  mhsReplEval,
  mhsReplRun,
  ) where
import qualified Prelude(); import MHSPrelude
import Control.Exception(try, SomeException, displayException, evaluate)
import Control.Monad(unless)
import Data.Char(isSpace)
import Data.IORef
import Foreign.C.String(CString, peekCString, peekCStringLen, newCString, newCStringLen)
import Foreign.C.Types(CInt, CSize)
import Foreign.Ptr(Ptr, nullPtr)
import Foreign.StablePtr
import Foreign.Storable(poke)
import MicroHs.Compile(Cache, compileModuleP, compileToCombinators, emptyCache, getMhsDir)
import MicroHs.CompileCache(cachedModules)
import MicroHs.Desugar(LDef)
import MicroHs.Exp(Exp(Var))
import MicroHs.Expr(EModule(..), ImpType(..))
import MicroHs.Flags(Flags, defaultFlags)
import MicroHs.Ident(Ident, mkIdent, qualIdent, showIdent)
import MicroHs.Parse(parse, pTopModule)
import MicroHs.StateIO(runStateIO)
import MicroHs.TypeCheck(TModule(..))
import MicroHs.Translate(translateMap, translateWithMap)
import System.IO.Error(userError)
import Unsafe.Coerce(unsafeCoerce)

-- REPL-context exports ------------------------------------------------------

type ReplHandle = StablePtr (IORef ReplCtx)

foreign export ccall "mhs_repl_new"    mhsReplNew    :: IO ReplHandle
foreign export ccall "mhs_repl_free"   mhsReplFree   :: ReplHandle -> IO ()
foreign export ccall "mhs_repl_define" mhsReplDefine :: ReplHandle -> CString -> CSize -> Ptr CString -> IO CInt
foreign export ccall "mhs_repl_eval"   mhsReplEval   :: ReplHandle -> CString -> CSize -> Ptr CString -> Ptr CSize -> Ptr CString -> IO CInt
foreign export ccall "mhs_repl_run"    mhsReplRun    :: ReplHandle -> CString -> CSize -> Ptr CString -> IO CInt

-- Internal data -------------------------------------------------------------

data ReplCtx = ReplCtx {
  rcFlags :: Flags,
  rcCache :: Cache,
  rcDefs  :: String
  }

moduleHeader :: String
moduleHeader = "module Inline where\n\
               \import Prelude\n\
               \import System.IO.PrintOrRun\n\
               \default Num (Integer, Double)\n\
               \default IsString (String)\n\
               \default Show (())\n"

evalResultName, runResultName :: String
evalResultName = "evalResult"
runResultName  = "runResult"

evalResultIdent, runResultIdent :: Ident
evalResultIdent = mkIdent evalResultName
runResultIdent  = mkIdent runResultName

-- REPL context management ---------------------------------------------------

mhsReplNew :: IO ReplHandle
mhsReplNew = do
  ctx <- initialCtx
  ref <- newIORef ctx
  newStablePtr ref

mhsReplFree :: ReplHandle -> IO ()
mhsReplFree = freeStablePtr

mhsReplDefine :: ReplHandle -> CString -> CSize -> Ptr CString -> IO CInt
mhsReplDefine ctxHandle srcPtr srcLen errPtr =
  withCtx ctxHandle $ \ref -> do
    source <- peekSource srcPtr srcLen
    ctx <- readIORef ref
    res <- replDefine ctx source
    case res of
      Left err -> failWith err errPtr
      Right ctx' -> writeIORef ref ctx' >> success

mhsReplEval :: ReplHandle -> CString -> CSize -> Ptr CString -> Ptr CSize -> Ptr CString -> IO CInt
mhsReplEval ctxHandle srcPtr srcLen outPtr outLenPtr errPtr =
  withCtx ctxHandle $ \ref -> do
    source <- peekSource srcPtr srcLen
    ctx <- readIORef ref
    res <- replEval ctx source
    case res of
      Left err -> failEval err
      Right (txt, ctx') -> do
        (buf, len) <- newCStringLen txt
        poke outPtr buf
        poke outLenPtr (fromIntegral len)
        whenPtr errPtr $ poke errPtr nullPtr
        writeIORef ref ctx'
        return 0
  where
    failEval err = do
      whenPtr outPtr $ poke outPtr nullPtr
      whenPtr outLenPtr $ poke outLenPtr 0
      failWith err errPtr

mhsReplRun :: ReplHandle -> CString -> CSize -> Ptr CString -> IO CInt
mhsReplRun ctxHandle srcPtr srcLen errPtr =
  withCtx ctxHandle $ \ref -> do
    source <- peekSource srcPtr srcLen
    ctx <- readIORef ref
    res <- replRun ctx source
    case res of
      Left err -> failWith err errPtr
      Right (_, ctx') -> writeIORef ref ctx' >> success

-- Core evaluation -----------------------------------------------------------

replDefine :: ReplCtx -> String -> IO (Either String ReplCtx)
replDefine ctx snippet = do
  let snippet' = ensureTrailingNewline snippet
      defs' = rcDefs ctx ++ snippet'
      src = buildModule defs'
  compileModule ctx{rcDefs = defs'} src >>= \case
    Left err -> return (Left err)
    Right (_, cache') -> return (Right ctx{ rcDefs = defs', rcCache = cache' })

replEval :: ReplCtx -> String -> IO (Either String (String, ReplCtx))
replEval ctx expr = do
  let block = exprBlock expr
      src = buildModule (rcDefs ctx ++ block)
  compileModule ctx src >>= \case
    Left err -> return (Left err)
    Right (cmdl, cache') -> do
      res <- extractString cache' cmdl evalResultIdent
      return (Right (res, ctx{ rcCache = cache' }))

replRun :: ReplCtx -> String -> IO (Either String ((), ReplCtx))
replRun ctx stmt = do
  let block = runBlock stmt
      src = buildModule (rcDefs ctx ++ block)
  compileModule ctx src >>= \case
    Left err -> return (Left err)
    Right (cmdl, cache') -> do
      runAction cache' cmdl runResultIdent
      return (Right ((), ctx{ rcCache = cache' }))

-- Helpers -------------------------------------------------------------------

initialCtx :: IO ReplCtx
initialCtx = do
  dir <- getMhsDir
  let flags = defaultFlags dir
  return ReplCtx { rcFlags = flags, rcCache = emptyCache, rcDefs = "" }

buildModule :: String -> String
buildModule defs = moduleHeader ++ defs

exprBlock :: String -> String
exprBlock expr =
  evalResultName ++ " :: String\n" ++
  evalResultName ++ " = show (" ++ sanitizeExpr expr ++ ")\n"

runBlock :: String -> String
runBlock stmt =
  runResultName ++ " :: IO ()\n" ++
  runResultName ++ " = _printOrRun (" ++ sanitizeExpr stmt ++ ")\n"

sanitizeExpr :: String -> String
sanitizeExpr = map replChar
  where
    replChar c | c == '\n' || c == '\r' = ' '
               | otherwise               = c

ensureTrailingNewline :: String -> String
ensureTrailingNewline s
  | null s = "\n"
  | last s == '\n' = s
  | otherwise = s ++ "\n"

compileModule :: ReplCtx -> String -> IO (Either String (TModule [LDef], Cache))
compileModule ctx src =
  case parse pTopModule "<repl>" src of
    Left err -> return (Left err)
    Right mdl -> do
      res <- try (runStateIO (compileModuleP (rcFlags ctx) ImpNormal mdl) (rcCache ctx))
      case res of
        Left e -> return (Left (displayException (e :: SomeException)))
        Right (((dmdl, _, _, _, _), _), cache') ->
          return (Right (compileToCombinators dmdl, cache'))

extractString :: Cache -> TModule [LDef] -> Ident -> IO String
extractString cache cmdl ident = do
  let defs = tBindingsOf cmdl
      baseMap = translateMap $ concatMap tBindingsOf (cachedModules cache)
      val = translateWithMap baseMap (defs, Var qIdent)
      str = unsafeCoerce val :: String
  evaluate (length str) >> return str
  where
    qIdent = qualIdent (tModuleName cmdl) ident

runAction :: Cache -> TModule [LDef] -> Ident -> IO ()
runAction cache cmdl ident = do
  let defs = tBindingsOf cmdl
      baseMap = translateMap $ concatMap tBindingsOf (cachedModules cache)
      actionAny = translateWithMap baseMap (defs, Var qIdent)
      action = unsafeCoerce actionAny :: IO ()
  action
  where
    qIdent = qualIdent (tModuleName cmdl) ident

peekSource :: CString -> CSize -> IO String
peekSource ptr len
  | ptr == nullPtr = return ""
  | len == 0       = peekCString ptr
  | otherwise      = peekCStringLen (ptr, fromIntegral len)

whenPtr :: Ptr a -> IO () -> IO ()
whenPtr ptr act = unless (ptr == nullPtr) act

success :: IO CInt
success = return 0

failWith :: String -> Ptr CString -> IO CInt
failWith msg errPtr = do
  whenPtr errPtr $ do
    errBuf <- newCString msg
    poke errPtr errBuf
  return (-1)

withCtx :: ReplHandle -> (IORef ReplCtx -> IO CInt) -> IO CInt
withCtx handle action = action =<< deRefStablePtr handle

