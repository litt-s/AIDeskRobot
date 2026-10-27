CREATE OR REPLACE FUNCTION public.aidesk_query(statements jsonb) RETURNS jsonb
LANGUAGE plpgsql SECURITY INVOKER
SET search_path = aidesk, pg_temp
SET standard_conforming_strings = on
SET statement_timeout = '30s'
AS $body$
DECLARE
  statement jsonb;
  query_text text;
  result_rows jsonb;
  changed bigint;
  output jsonb := '[]'::jsonb;
BEGIN
  IF auth.role() IS DISTINCT FROM 'service_role' THEN
    RAISE EXCEPTION 'forbidden' USING ERRCODE = '42501';
  END IF;
  IF jsonb_typeof(statements) IS DISTINCT FROM 'array' OR jsonb_array_length(statements) NOT BETWEEN 1 AND 100 THEN
    RAISE EXCEPTION 'invalid statements';
  END IF;
  FOR statement IN SELECT jsonb_array_elements(statements) LOOP
    query_text := statement->>'sql';
    IF query_text !~* '^\s*(SELECT|INSERT|UPDATE|DELETE)\s' OR length(query_text) > 2000000 THEN
      RAISE EXCEPTION 'invalid statement';
    END IF;
    IF (statement->>'returns_rows')::boolean THEN
      EXECUTE 'WITH aidesk_rpc_rows AS (' || query_text || ') SELECT coalesce(jsonb_agg(to_jsonb(aidesk_rpc_rows)), ''[]''::jsonb) FROM aidesk_rpc_rows' INTO result_rows;
      changed := jsonb_array_length(result_rows);
    ELSE
      EXECUTE query_text;
      GET DIAGNOSTICS changed = ROW_COUNT;
      result_rows := '[]'::jsonb;
    END IF;
    output := output || jsonb_build_array(jsonb_build_object('success', true, 'results', result_rows, 'meta', jsonb_build_object('changes', changed)));
  END LOOP;
  RETURN output;
END;
$body$;
REVOKE ALL ON FUNCTION public.aidesk_query(jsonb) FROM PUBLIC, anon, authenticated;
GRANT EXECUTE ON FUNCTION public.aidesk_query(jsonb) TO service_role;
NOTIFY pgrst, 'reload schema';
