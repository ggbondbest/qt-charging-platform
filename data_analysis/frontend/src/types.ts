export type JsonObject = Record<string, any>;
export interface Location {
  latitude: number;
  longitude: number;
}
export interface City extends Location {
  cityId: string;
  cityName: string;
}
export interface Station extends Location {
  stationId: string;
  cityId: string;
  stationName: string;
  capacity: number;
  powerKw: number;
  pricePerKwh: number;
  currentFree?: number;
  queued?: number;
  committed?: number;
  charging?: number;
  backgroundBusy?: number;
}
export interface Clock {
  time: string;
  speed: number;
  paused: boolean;
}
export interface Bootstrap {
  cities: City[];
  stations: Station[];
  clock: Clock;
  modelStatus: JsonObject;
  provenance: JsonObject;
}
export interface User {
  userId: string;
  name: string;
  points: number;
}
export interface Candidate extends Station {
  rank: number;
  score: number;
  etaMinutes: number;
  distanceKm: number;
  routeSource: string;
  currentFree: number;
  expectedFree: number;
  availableProbability: number;
  waitMinutes: number;
  waitP90Minutes: number;
  serviceProbability: number;
  arrivalTime: string;
  forecastTime: string;
  resolutionMinutes: number;
  loadRatio: number;
  rewardPoints: number;
  scoreBreakdown: Record<string, number>;
  reasons: string[];
  totalMinutes: number;
}
export interface Recommendation {
  recommendationId: string;
  createdAt: string;
  expiresAt: string;
  referenceTime: string;
  origin: Location;
  candidates: Candidate[];
  nearestComparison?: JsonObject;
  warnings: string[];
}
export type TripStatus =
  | "EN_ROUTE"
  | "QUEUED"
  | "CALLED"
  | "RESERVED"
  | "CHARGING"
  | "PENDING_PAYMENT"
  | "COMPLETED"
  | "CANCELLED"
  | "EXPIRED";
export interface Trip {
  tripId: string;
  stationId: string;
  stationName: string;
  status: TripStatus;
  origin?: Location;
  queuePosition?: number;
  peopleAhead?: number;
  callExpiresAt?: string;
  reservationExpiresAt?: string;
  arrivalEligibleAt?: string;
  energyKwh: number;
  amount: number;
  chargingSeconds: number;
  rewardPoints: number;
  awardedPoints: number;
  stopReason?: string;
  events: JsonObject[];
}
export interface Me {
  user: User;
  trips: Trip[];
  ledger: JsonObject[];
}
export interface Envelope<T> {
  data: T;
  meta?: { dataSource: string; clockTime: string; mode: string };
  error?: { code: string; message: string };
}
