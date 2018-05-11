{-# LANGUAGE TupleSections #-}
import Control.Monad
import Control.Monad.State
import Data.List (nub, intersect, intersperse, intercalate, sort)
import Data.Maybe
import Data.Bits
import Data.Char
import Text.Megaparsec
import Text.Megaparsec.Char
import qualified Text.Megaparsec.Char.Lexer as L
import Debug.Trace
import System.Environment
import System.Exit

type Parser = Parsec () String

data InfInt = Fin Int | Inf deriving (Show, Eq)

instance Ord InfInt where
  Inf <= Inf     = True
  Inf <= Fin _   = False
  Fin _ <= Inf   = True
  Fin a <= Fin b = a <= b

instance Num InfInt where
  Fin a + Fin b  = Fin (a+b)
  _ + _          = Inf
  Fin a * Fin b  = Fin (a*b)
  _ * _          = Inf
  abs (Fin a)    = Fin (abs a)
  abs Inf        = Inf
  negate (Fin a) = Fin (negate a)
  negate _       = undefined
  signum (Fin 0) = 0
  signum _       = 1
  fromInteger x  = Fin (fromInteger x)

type Constraint = (InfInt, InfInt)       -- lower bound, upper bound
type Member = (String,Type)
data Type = TRef String                  -- reference another type by name (circular ref best avoided ...)
          | TConstr Constraint Type      -- type is constrained to be in range (min,max) -- only for TNat and TByte
          | TNat                         -- variable-length natural number
          | TByte                        -- one byte
          | TFlag Int                    -- a flag in the preceding header field, bit 0 is lsb
          | THeader Int Type             -- one-byte header; bits 0 .. n-1 are occupied by type, rest is for flags
          | TEnum [(String, Int)]        -- bijective mapping of symbol to natural number
          | TSeq Type                    -- sequence of type
          | TRecord [Member]             -- a record of named fields, serialised in order
          | TCase String [([Case],Type)] -- variant record, discriminated by named field preceding it in same record
          deriving (Show)
data Case = CEnum String                 -- cases are be enum symbols if discriminant is an enumerated type
          | CInt (Int,Int)               -- else ranges (inclusive) of natural numbers
          deriving (Show, Eq)

specification = sc *> definitions <* eof
definitions   = concat <$> many definition
definition    = (\ns t -> map (,t) ns) <$> sepBy1 identifier comma <*> (symbol "=" *> atype)
atype         = mkConstr <$> atypeUnconstr <*> many (symbol "range" *> range)
  where range = (,) <$> integer <*> (symbol "..." *> optional integer)
        mkConstr t cs = foldl mkConstr' t cs
        mkConstr' t (a,Nothing) = TConstr (Fin a, Inf) t
        mkConstr' t (a,Just b)  = TConstr (Fin a, Fin b) t
atypeUnconstr = typeDef <|> typeRef <|> atype
typeDef       = typePrim <|> typeSeq <|> typeHeader <|> typeEnum <|> typeRecord <|> typeCase <|> typeOption
typeRef       = TRef       <$> identifier
typePrim      = pure TNat  <$> reserved "nat"
            <|> pure TByte <$> reserved "byte"
            <|> TFlag      <$> (reserved "flag" *> integer)
typeSeq       = TSeq       <$> brackets atype
typeHeader    = THeader    <$> (reserved "header" *> integer) <*> atype
typeEnum      = TEnum      <$> (reserved "enum" *> braces (sepBy1 enumSym comma))
enumSym       = (,)        <$> identifier <*> (symbol "=" *> integer)
typeRecord    = TRecord    <$> (reserved "record" *> braces (sepBy1 recordMember comma))
recordMember  = (,)        <$> identifier <*> (symbol ":" *> atype)
typeCase      = TCase      <$> (reserved "case" *> identifier <* reserved "of") <*> braces (sepBy1 labelCase comma)
labelCase     = (,)        <$> sepBy1 constantRange comma <*> (symbol "->" *> atype)
constantRange = mkCInt     <$> integer <*> optional (symbol "..." *> integer)
            <|> CEnum      <$> identifier
  where mkCInt a Nothing  = CInt (a,a)
        mkCInt a (Just b) = CInt (a,b)
typeOption    = do
  reserved "if"
  flag <- atype
  reserved "then"
  t <- atype
  return $ TRecord [("present", flag), ("value", TCase "present" [([CEnum "true"], t)])]

sc :: Parser ()
sc = L.space (void spaceChar) lineComment blockComment
  where lineComment  = L.skipLineComment "--"
        blockComment = L.skipBlockComment "{-" "-}"
lexeme     = L.lexeme sc
symbol     = L.symbol sc
braces     = between (symbol "{") (symbol "}")
brackets   = between (symbol "[") (symbol "]")
integer    = lexeme L.decimal >>= return . fromIntegral
comma      = symbol ","
reserved w = string w *> notFollowedBy alphaNumChar *> sc
identifier = L.lexeme sc (p >>= check)
  where p = (:) <$> (letterChar <|> char '_') <*> many (alphaNumChar <|> char '_')
        check x = do when (x `elem` rws) $ fail ("keyword " ++ show x ++ " cannot be an identifier") ; return x
        rws = [ "byte", "case", "enum", "flag", "header", "if", "of", "record", "nat" ]

parseType :: String -> Either String [(String,Type)]
parseType idl = case parse specification "(anonymous)" idl of
  Left err -> Left $ show err
  Right defs -> case checkDefs defs of
    Left err' -> Left $ err' ++ " for " ++ show defs
    Right _ -> Right defs

checkDefs :: [(String,Type)] -> Either String ()
checkDefs defs
  | not $ null $ dupsfst defs = fail $ "duplicate definitions " ++ (show . dupsfst) defs
  | isNothing toplevel = fail "top-level type Message not defined"
  | otherwise          = check2 [] [] 8 (fromJust toplevel) >> return ()
  where
    toplevel = lookup "Message" defs
    dups xs = filter (\x -> (length $ filter (== x) xs) > 1) xs
    dupsfst = dups . map fst
    -- check2 computes list of allowed bit positions for flags & whether a new header was seen
    -- for a type, given:
    -- - precms: members of parent record preceding this (if any -- for discriminator)
    -- - aflags: allowed flags, list of bit positions where flags can go
    -- - width:  number of bits an enum may occupy -- usually 8, narrowed when in header
    check2 :: [(String,Type)] -> [Int] -> Int -> Type -> Either String ([Int],Bool)
    check2 precms aflags width t =
      case t of
        TRef n -> do
          let t' = lookup n defs
          when (isNothing t') $ fail ("type " ++ n ++ " undefined")
          check2 precms aflags width (fromJust t')
        TConstr (a,b) t' -> do
          when (a < 0 || b < a) $ fail ("invalid range " ++ show a ++ " ... " ++ show b)
          res <- check2 precms aflags width t'
          checkConstraint a b t'
          return res
        TNat -> return (aflags, False)
        TByte -> return (aflags, False)
        TFlag n -> do
          when (n < 0 || n > 7) $ fail ("flag " ++ show n ++ " out of range")
          when (n `notElem` aflags) $ fail ("flag " ++ show n ++ " not allowed in this context " ++ show aflags)
          return (filter (/= n) aflags, False)
        THeader n t -> do
          when (n < 1 || n > 8) $ fail "header field to narrow or too wide"
          check2 [] [] n t
          checkHeader t
          return (allowFlags n, True)
        TEnum xs -> do
          when ((length . nub . map fst) xs /= length xs) $ fail "duplicate enum symbols"
          when ((not . null . intersect ["false","true"] . map fst) xs) $ fail "enum may not define false or true"
          when ((length . nub . map snd) xs /= length xs) $ fail "duplicate enum values"
          let outOfRange = [ n | (n,v) <- xs, v < 0 || v >= 2^width ]
          when ((not . null) outOfRange) $ fail ("enum symbols " ++ show outOfRange ++ " out of range")
          return (aflags, False)
        TSeq t -> do
          (aflags', containsHeader) <- check2 [] [] width t
          if containsHeader then return ([], True) else return (aflags, False)
        TRecord ms -> do
          when ((length . nub . map fst) ms /= length ms) $ fail "duplicate record members"
          checkMembers aflags ms
        TCase d cs -> do
          let mt = lookup d precms
          when (isNothing mt) $ fail ("discriminant " ++ d ++ " not among preceding members in parent record")
          let mdisct = checkDisc (fromJust mt)
          when (isNothing mdisct) $ fail ("invalid discriminant type " ++ d)
          let (disct, constr) = fromJust mdisct
          checkCases disct constr (concatMap fst cs)
          aflags' <- mapM (check2 [] aflags width . snd) cs
          let containsHeader = or (map snd aflags')
              aflags'' = map (\(a,r) -> if r then [] else a) aflags'
          return (foldr intersect aflags aflags'', containsHeader)
    allowFlags n = [n .. 7]
    checkMembers aflags ms = liftM fst $ foldM cm ((aflags,False),[]) ms
      where
        cm ((af,ch),ms) (n,t) = do
          (af',ch') <- check2 ms af 8 t
          return ((af', ch || ch'), (n,t):ms)
    checkDisc :: Type -> Maybe (Type, Constraint)
    checkDisc t = case t of
                    TRef t'      -> checkDisc $ fromJust $ lookup t' defs
                    TConstr x t' -> do (t'', _) <- checkDisc t' ; return (t'', x)
                    TNat         -> Just (t, (Fin 0, Inf))
                    TByte        -> Just (t, (Fin 0, Fin 255))
                    TFlag _      -> Just (t, (Fin 0, Inf))
                    TEnum _      -> Just (t, (Fin 0, Inf))
                    THeader _ t' -> checkDisc t'
                    _            -> Nothing
    checkHeader t = case t of
                      TRef t'  -> checkHeader $ fromJust $ lookup t' defs
                      TByte    -> return ()
                      TEnum _  -> return ()
                      _        -> fail $ "invalid header type " ++ show t
    checkConstraint a b t = case t of
                              TRef t' -> checkConstraint a b $ fromJust $ lookup t' defs
                              TByte   -> when (b > 255) $ fail "range constraint invalid for Byte"
                              TNat    -> return ()
                              TSeq _  -> return () 
                              TConstr (a',b') t' -> do
                                when (a < a' || b' > b) $ fail "attempt to widen range constraint"
                                checkConstraint a' b' t'
                              _ -> fail "range constraint applied to non-integral and non-sequence type"
    checkCases :: Type -> Constraint -> [Case] -> Either String ()
    checkCases (TEnum xs) _ cs   = void $ checkCEnum xs cs
    checkCases TNat x cs         = void $ checkCInt x cs
    checkCases TByte x cs        = void $ checkCInt x cs
    checkCases (TFlag _) _ cs    = void $ checkCEnum [("false",0),("true",1)] cs
    checkCEnum xs cs = foldM chk [] cs
      where
        chk _   (CInt _)  = fail "enum discriminant with integer case"
        chk acc (CEnum n) = do
          let c = lookup n xs ; v = fromJust c
          when (isNothing c) $ fail ("undefined enum symbol " ++ n ++ " in case")
          when (v `elem` acc) $ fail ("duplicate case " ++ n)
          return (v : acc)
    checkCInt (min,max) cs = foldM (chk min max) [] cs
      where
        chk _ _ _       (CEnum _)    = fail "integer discriminant with enum case"
        chk min max acc (CInt (l,h)) = do
          when (l > h) $ fail ("invalid range [" ++ show l ++ "," ++ show h ++ "]")
          when (Fin l < min || Fin h > max) $ fail ("case " ++ show (l,h) ++ " out of range")
          when ((l,h) `intvElem` acc) $ fail ("overlap in cases [" ++ show l ++ "," ++ show h ++ "]")
          return ((l,h) : acc)
    intvElem (a,b) ivs = (not . null) ivs && and (map (\(c,d) -> a <= d && b >= c) ivs)

data PElt = PHeader Int [(Int,String)] (String, Maybe String) -- width | flag names | lower part (+ note)
          | PByte String       -- a single byte + label
          | PNat String        -- a number + label
          | PSeq String        -- a sequence + name
          | PFlag Int String   -- a flag, serialised in the preceding header
          | PIf String PElt    -- PElt conditional on String
          deriving (Show, Eq)

center :: Int -> String -> String
center width str
  | length str >= width = take width str
  | otherwise           = take left (repeat ' ') ++ str ++ take right (repeat ' ') 
  where
    left  = (width - length str) `div` 2
    right = (width - length str + 1) `div` 2

type Defs = [(String,Type)]

simpleCase :: Defs -> Type -> Bool
simpleCase defs (TCase _ cs) = length cs <= 1 && (and $ map (simpleCase defs . snd) cs)
simpleCase defs (TRecord ms) = and $ map (simpleCase defs) $ map snd ms
simpleCase defs (TRef tn) = simpleCase defs $ fromJust $ lookup tn defs
simpleCase defs (TSeq t) = simpleCase defs t
simpleCase _ _ = True

data Value = VEnum String Int | VFlag Int Bool deriving (Eq)
newtype Env = Env { unEnv :: [([String],Value)] } deriving (Show, Eq)
data SimpleT = STFlag | STPrim | STComplex deriving (Show, Eq)

instance Show Value where
  show (VEnum x y) = x
  show (VFlag _ x) = if x then "true" else "false"

unwrapTypeH :: Defs -> Type -> Type
unwrapTypeH defs (TRef tn) = fromJust $ lookup tn defs
unwrapTypeH defs (THeader _ t) = unwrapTypeH defs t
unwrapTypeH defs (TConstr _ t) = unwrapTypeH defs t
unwrapTypeH _ t = t

unwrapTypeFN :: Defs -> Type -> Type
unwrapTypeFN defs (TRef tn) = case unwrapTypeH defs $ fromJust $ lookup tn defs of
  TFlag _ -> (TRef tn)
  t       -> t
unwrapTypeFN defs t = unwrapTypeH defs t

splitComplex :: Defs -> [String] -> [Member] -> Type -> [Env]
splitComplex defs pfx ms typ = case typ of
  TRef tn     -> splitComplex defs pfx ms $ fromJust $ lookup tn defs
  TConstr _ t -> splitComplex defs pfx ms t
  TSeq t      -> splitComplex defs pfx [] t
  TRecord ms  -> concatMap (\(n,t) -> splitComplex defs (n:pfx) ms t) ms
  TCase d cs  -> scc ms d cs
  _           -> []
  where
    scc :: [Member] -> String -> [([Case],Type)] -> [Env]
    scc ms d cs | simple cs = splitComplex defs ("<>":pfx) [] (snd $ head cs)
                | otherwise = concatMap (makeEnv ms d) cs
    simple :: [([Case],Type)] -> Bool
    simple cs | length cs == 1 = True
              | otherwise      = (foldr g STFlag $ map (s . snd) cs) /= STComplex
              where
                g x STFlag    = x
                g x STPrim    = if x == STFlag then STPrim else STComplex
                g _ STComplex = STComplex
                s t = case t of
                        TFlag _     -> STFlag
                        TRef tn     -> s $ fromJust $ lookup tn defs
                        TConstr _ t -> s t
                        TNat        -> STPrim
                        TByte       -> STPrim
                        TRecord ms  -> foldr g STFlag $ map (s . snd) ms
                        _           -> STComplex
    makeEnv :: [Member] -> String -> ([Case],Type) -> [Env]
    makeEnv ms d (CEnum e : _, t)
      | null subenvs = [self]
      | otherwise    = map (concatEnv self) subenvs
      where subenvs = splitComplex defs (("<"++e++">"):pfx) [] t
            disct   = unwrapTypeH defs $ fromJust $ lookup d ms
            self    = case disct of
                        TFlag p  -> Env [(d:tail pfx, VFlag p (e == "true"))]
                        TEnum es -> Env [(d:tail pfx, VEnum e (maybe 0 id $ lookup e es))]
                        _        -> trace (show disct) $ Env []
            concatEnv (Env a) (Env b) = Env (a++b)

type Pic = [PElt]
genPic :: Defs -> Type -> [Pic]
genPic defs typ = if null envs then [genPic1 defs typ (Env [])] else nub $ map (genPic1 defs typ) envs
  where
    envs = splitComplex defs [] [] typ

traceZ x = trace (show x) x

-- wouldn't foldr do the trick nicely?
-- - each entry in type yields any number of entries for the picture, which can be cons'd
-- - flags are collected, and squished into a header
-- - the type name and field prefix are determined by recursion, so that wouldn't interfere
genPic1 :: Defs -> Type -> Env -> Pic
genPic1 defs typ env = collectFlags $ gen [] [] Nothing typ env []
  where
    gen :: [Member] -> [String] -> Maybe String -> Type -> Env -> Pic -> Pic
    gen members pfx typnam typ env acc = case typ of
      TRef tn     -> gen members pfx (Just tn) (fromJust $ lookup tn defs) env acc
      TFlag p     -> PFlag p v : acc
        where
          v = case flagInEnv p env of
                         Just v  -> show $ fromEnum v
                         Nothing -> if isJust typnam then fromJust typnam else head pfx
      TConstr _ t -> gen members pfx typnam t env acc
      TNat        -> acc ++ [PNat $ fieldname pfx]
      TByte       -> acc ++ [PByte $ fieldname pfx]
      TEnum _     -> acc ++ [PNat $ fst $ valOrName pfx env ]
      TRecord ms  -> foldl (\acc' (n,t) -> gen ms (n:pfx) Nothing t env acc') acc ms
      THeader w t -> acc ++ [PHeader w [] (valOrName pfx env)]
      TSeq t      -> acc ++ [PSeq (typename t pfx)]
      TCase d cs  -> case lookup (d:tail pfx) (unEnv env) of
                       Nothing ->
                         let eltsX :: [(PElt -> PElt, Pic)]
                             eltsX = map (\(c,t) -> (PIf $ makeCond members d c, gen [] pfx Nothing t env [])) cs
                             flags = filter isFlag $ concatMap snd eltsX
                             isFlag x = case x of { PFlag _ _ -> True ; _ -> False }
                             elts = map (\(cond, es) -> (cond, filter (not . isFlag) es)) eltsX
                             eltsCond = concatMap (\(cond, es) -> map cond es) elts
                         in
                           flags ++ acc ++ eltsCond
                       Just x -> gen [] (("<"++show x++">"):pfx) Nothing (findCase cs $ show x) env acc
    flagInEnv p env = msum $ map (\(_,v) -> case v of
                                              VFlag p' v' -> if p' == p then Just v' else Nothing
                                              _ -> Nothing) (unEnv env)
    makeCond :: [Member] -> String -> [Case] -> String
    makeCond ms d cs = case dt of
      TRef tn -> makeFlagCond tn cs
      TEnum _ -> makeEnumCond d cs
      TNat    -> makeIntCond d cs
      TByte   -> makeIntCond d cs
      where
        dt = unwrapTypeFN defs $ fromJust $ lookup d ms
        makeFlagCond name cs
          | t `elem` cs && f `elem` cs = "true"
          | t `elem` cs = name
          | otherwise = "not " ++ name
          where t = CEnum "true"
                f = CEnum "false"
        makeEnumCond name [CEnum c] = name ++ " = " ++ c
        makeEnumCond name cs = name ++ " in {" ++ intercalate "," csnames ++ "}"
          where csnames = map (\(CEnum n) -> n) cs
        makeIntCond :: String -> [Case] -> String
        makeIntCond name [CInt (a,b)]
          | a == b    = name ++ " = " ++ show a
          | otherwise = show a ++ " <= " ++ name ++ " <= " ++ show b
        makeIntCond name cs = intercalate " OR " $ map (\c -> makeIntCond name [c]) cs
    findCase :: [([Case],Type)] -> String -> Type
    findCase cs v = fromJust $ lookup (CEnum v) $ makeLookupTable cs
      where
        makeLookupTable cs = concatMap (\(cs',t) -> map (,t) cs') cs
    typename :: Type -> [String] -> String
    typename typ xfieldname = case typ of
      TRef tn     -> fieldname xfieldname -- tn
      TConstr _ t -> typename t xfieldname
      TRecord ms  -> concat $ intersperse "," $ map fst ms
      _           -> fieldname xfieldname
    fieldname = head . dropWhile ugly
      where
        -- hack takes care of union discriminants and the case where there is a record with only a
        -- "value", as happens from desugaring "if A then B"; the better way would of course be to
        -- recognize the full pattern ...
        ugly x = head x == '<' || x == "value"
    valOrName :: [String] -> Env -> (String, Maybe String)
    valOrName name env = case lookup name (unEnv env) of
      Nothing          -> (head name, Nothing)
      Just (VEnum e v) -> (show v, Just e)
    collectFlags :: Pic -> Pic
    collectFlags pic = reverse $ f [] [] pic
      where
        f acc fs [] = acc ++ map (\(p,v) -> PFlag p v) fs
        f acc fs ((PFlag p v):ps) = f acc ((p,v) : fs) ps
        f acc fs ((PHeader w [] n):ps) = f (PHeader w fs n : acc) [] ps
        f acc fs (p:ps) = f (p : acc) fs ps

pretty :: PElt -> String
pretty (PHeader w fs (n, note)) = case note of
  Nothing -> base
  Just x  -> base ++ " (" ++ x ++ ")"
  where x p = maybe "X" id $ lookup p fs
        width = 2*w-1
        base = "|" ++ concat [ x p ++ "|" | p <- reverse [w .. 7] ] ++ center width n ++ "|"
pretty (PNat n) = "~" ++ center 15 n ++ "~"
pretty (PByte n) = "|" ++ center 15 n ++ "|"
pretty (PSeq n) = "~" ++ center 15 n ++ "~"
pretty (PFlag p v) = "XXX" ++ show (p,v) ++ "XXX"
pretty (PIf c p) = pretty p ++ " if " ++ c

genText :: Defs -> Type -> [(String, String)]
genText defs typ = merge $ map genText1 $ genPic defs typ
  where
    genText1 :: Pic -> (String, String)
    genText1 pic = (ident, unlines $ sep width line (head withsep) : withsep)
      where withsep = addseps $ map pretty pic
            ident1 = msum $ map (\x -> case x of { PHeader _ _ (_,n) -> n ; _ -> Nothing }) pic
            ident = if isJust ident1 then fromJust ident1 else "anon"
    addseps :: [String] -> [String]
    addseps [l] = l : [sep width l line]
    addseps (l:g:rest) = l : sep width l g : addseps (g:rest)
    width = length line
    line = "-----------------"
    sep :: Int -> String -> String -> String
    sep 0 _ _ = ""
    sep _ _ [] = ""
    sep _ [] _ = ""
    sep n (a:above) (b:below)
      | a `elem` "|~" || b `elem` "|~" = '+' : rest
      | otherwise = '-' : rest
      where rest = sep (n-1) above below
    merge :: [(String, String)] -> [(String, String)]
    merge xs = merge' [] xs
    merge' acc [] = reverse acc
    merge' acc (x:xs) = case lookup (fst x) acc of
      Nothing -> merge' (x:acc) xs
      Just y  -> let (pre, match:post) = span (\(i,_) -> i /= fst x) acc in merge' (pre ++ [(fst x, y ++ snd x)] ++ post) xs

putdef (ident, def) = do
  putStrLn $ "\\begin{SaveVerbatim}{pict" ++ ident ++ "}"
  putStr def
  putStrLn "\\end{SaveVerbatim}"

main = do
  inp <- getContents
  let pres = parseType inp
  case pres of
    Left err -> do putStrLn err ; exitFailure
    _ -> return ()
  let Right defs = pres
  putStrLn "% ======== MESSAGES ========"
  mapM_ putdef $ genText defs (fromJust $ lookup "Message" defs)
  putStrLn "% ====== DECLARATIONS ======"
  mapM_ putdef $ genText defs (fromJust $ lookup "Declaration" defs)
  exitSuccess
