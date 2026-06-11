declare module "tz-lookup" {
  /** IANA timezone name for a coordinate, e.g. "America/Los_Angeles". */
  export default function tzLookup(lat: number, lon: number): string;
}
